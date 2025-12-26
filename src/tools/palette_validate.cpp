#include "core/palette/palette.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
static std::string Trim(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && is_space((unsigned char)s.back()))
        s.pop_back();
    return s;
}

static std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool ParseHexRgb(std::string_view s, phos::color::Rgb8& out)
{
    if (!s.empty() && s[0] == '#')
        s.remove_prefix(1);
    // Accept RRGGBB or RRGGBBAA (ignore alpha).
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    auto byte_at = [&](size_t i) -> int {
        const int hi = nyb(s[i + 0]);
        const int lo = nyb(s[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        return (hi << 4) | lo;
    };

    const int r = byte_at(0);
    const int g = byte_at(2);
    const int b = byte_at(4);
    if (r < 0 || g < 0 || b < 0)
        return false;

    out.r = (std::uint8_t)r;
    out.g = (std::uint8_t)g;
    out.b = (std::uint8_t)b;
    return true;
}

static std::string Hex(const phos::color::Rgb8& c)
{
    auto hex2 = [](std::uint8_t v) -> std::string {
        static const char* k = "0123456789ABCDEF";
        std::string s;
        s.resize(2);
        s[0] = k[(v >> 4) & 0xFu];
        s[1] = k[v & 0xFu];
        return s;
    };
    return std::string("#") + hex2(c.r) + hex2(c.g) + hex2(c.b);
}

static void PrintUsage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " [--assets <dir>]\n"
              << "\n"
              << "Validates that built-in (static) palettes in src/ match any corresponding\n"
              << "entries present in assets/color-palettes.json (colors + ordering).\n"
              << "\n"
              << "Note: builtins are no longer required to be listed in color-palettes.json;\n"
              << "missing builtin entries are treated as OK.\n"
              << "\n"
              << "Options:\n"
              << "  --assets <dir>  Project assets dir (default: ./assets)\n";
}

struct JsonPalette
{
    std::string title;
    std::vector<phos::color::Rgb8> rgb;
};

static bool LoadJsonPalettes(const fs::path& json_path,
                             std::unordered_map<std::string, JsonPalette>& out_by_norm_title,
                             std::string& out_error)
{
    out_error.clear();
    out_by_norm_title.clear();

    std::ifstream f(json_path);
    if (!f)
    {
        out_error = std::string("Failed to open ") + json_path.string();
        return false;
    }

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }

    if (!j.is_array())
    {
        out_error = "Expected top-level JSON array in color-palettes.json";
        return false;
    }

    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;
        if (!item.contains("title") || !item["title"].is_string())
            continue;
        if (!item.contains("colors") || !item["colors"].is_array())
            continue;

        JsonPalette p;
        p.title = item["title"].get<std::string>();

        for (const auto& c : item["colors"])
        {
            if (!c.is_string())
                continue;
            phos::color::Rgb8 col{};
            if (!ParseHexRgb(c.get<std::string>(), col))
                continue;
            p.rgb.push_back(col);
            if (p.rgb.size() >= phos::color::kMaxPaletteSize)
                break;
        }
        if (p.rgb.empty())
            continue;

        const std::string key = Lower(Trim(p.title));
        out_by_norm_title[key] = std::move(p);
    }

    return true;
}

static std::optional<JsonPalette> FindJsonPalette(const std::unordered_map<std::string, JsonPalette>& by_norm_title,
                                                  std::initializer_list<std::string_view> titles)
{
    for (std::string_view t : titles)
    {
        std::string key = Lower(Trim(std::string(t)));
        auto it = by_norm_title.find(key);
        if (it != by_norm_title.end())
            return it->second;
    }
    return std::nullopt;
}

static int CompareRgb(const std::vector<phos::color::Rgb8>& expected,
                      const std::vector<phos::color::Rgb8>& actual,
                      std::vector<std::string>& out_mismatches,
                      int max_mismatches_to_print = 16)
{
    int mismatches = 0;

    if (expected.size() != actual.size())
    {
        out_mismatches.push_back("size mismatch: expected " + std::to_string(expected.size()) + " colors, got " +
                                 std::to_string(actual.size()) + " colors");
        // Still compare common prefix to provide more useful diff output.
    }

    const size_t n = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < n; ++i)
    {
        const auto& e = expected[i];
        const auto& a = actual[i];
        if (e.r != a.r || e.g != a.g || e.b != a.b)
        {
            ++mismatches;
            if ((int)out_mismatches.size() < max_mismatches_to_print)
            {
                out_mismatches.push_back("index " + std::to_string(i) + ": expected " + Hex(e) + ", got " + Hex(a));
            }
        }
    }

    // Account for extra tail.
    if (expected.size() != actual.size())
        mismatches += (int)std::max(expected.size(), actual.size()) - (int)n;

    return mismatches;
}
} // namespace

int main(int argc, char** argv)
{
    fs::path assets_dir = fs::path("assets");

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto need = [&](const char* opt) -> std::string_view {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << opt << "\n";
                PrintUsage(argv[0]);
                std::exit(2);
            }
            return std::string_view(argv[++i]);
        };

        if (a == "--help" || a == "-h")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (a == "--assets")
        {
            assets_dir = fs::path(std::string(need("--assets")));
        }
        else
        {
            std::cerr << "Unknown arg: " << a << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    const fs::path json_path = assets_dir / "color-palettes.json";

    std::unordered_map<std::string, JsonPalette> json_by_title;
    std::string err;
    if (!LoadJsonPalettes(json_path, json_by_title, err))
    {
        std::cerr << "palette_validate: FAIL: " << (err.empty() ? "failed to load JSON" : err) << "\n";
        return 3;
    }

    phos::color::PaletteRegistry reg;

    struct Check
    {
        std::string label;
        phos::color::BuiltinPalette builtin = phos::color::BuiltinPalette::None;
        std::initializer_list<std::string_view> json_titles;
    };

    const Check checks[] = {
        {"VGA 8", phos::color::BuiltinPalette::Vga8, {"VGA 8"}},
        {"VGA 16", phos::color::BuiltinPalette::Vga16, {"VGA 16"}},
        {"Xterm 16", phos::color::BuiltinPalette::Xterm16, {"xterm 16", "Xterm 16"}},
        {"Xterm 240 Safe", phos::color::BuiltinPalette::Xterm240Safe, {"xterm 240", "Xterm 240"}},
        {"Xterm 256", phos::color::BuiltinPalette::Xterm256, {"xterm 256", "Xterm 256"}},
    };

    int total_mismatches = 0;
    int skipped_missing = 0;
    for (const auto& c : checks)
    {
        const auto json_pal = FindJsonPalette(json_by_title, c.json_titles);
        if (!json_pal.has_value())
        {
            // Builtins are no longer required to appear in assets/color-palettes.json.
            // We only validate parity when a corresponding entry exists.
            ++skipped_missing;
            continue;
        }

        const phos::color::PaletteInstanceId id = reg.Builtin(c.builtin);
        const phos::color::Palette* p = reg.Get(id);
        if (!p)
        {
            std::cerr << "palette_validate: FAIL: builtin palette not registered: " << c.label << "\n";
            ++total_mismatches;
            continue;
        }

        std::vector<std::string> diffs;
        const int mism = CompareRgb(json_pal->rgb, p->rgb, diffs);
        if (mism != 0)
        {
            total_mismatches += mism;
            std::cerr << "palette_validate: MISMATCH: " << c.label << " vs JSON \"" << json_pal->title << "\"\n";
            for (const auto& d : diffs)
                std::cerr << "  - " << d << "\n";
            if ((int)diffs.size() >= 16)
                std::cerr << "  - (more mismatches omitted)\n";
        }
    }

    if (total_mismatches == 0)
    {
        std::cout << "palette_validate: OK";
        if (skipped_missing > 0)
            std::cout << " (" << skipped_missing << " builtin palettes not present in JSON; skipped)";
        std::cout << "\n";
        return 0;
    }

    std::cerr << "palette_validate: FAIL (" << total_mismatches << " mismatches)\n";
    return 1;
}


