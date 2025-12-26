#include "core/palette/palette_catalog.h"

#include "core/color_system.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace phos::color
{
static bool ParseHexRgb(std::string_view s, Rgb8& out)
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

static bool RgbEquals(const std::vector<Rgb8>& a, const std::vector<Rgb8>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b)
            return false;
    }
    return true;
}

static std::string TrimCopy(std::string_view s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return std::string(s.substr(b, e - b));
}

static std::string RgbToHexRgb(const Rgb8& c)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)c.r, (int)c.g, (int)c.b);
    return std::string(buf);
}

static std::string MakeUniqueTitle(const nlohmann::json& j, std::string_view wanted)
{
    using nlohmann::json;

    const std::string base = TrimCopy(wanted.empty() ? std::string_view("Imported Palette") : wanted);

    auto title_exists = [&](const std::string& t) -> bool
    {
        if (!j.is_array())
            return false;
        for (const auto& item : j)
        {
            if (!item.is_object())
                continue;
            auto it = item.find("title");
            if (it != item.end() && it->is_string() && it->get<std::string>() == t)
                return true;
        }
        return false;
    };

    if (!title_exists(base))
        return base;

    for (int n = 2; n < 10000; ++n)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (%d)", base.c_str(), n);
        std::string cand(buf);
        if (!title_exists(cand))
            return cand;
    }
    return base; // fallback
}

void PaletteCatalog::RebuildBuiltinList()
{
    m_ui_list.clear();
    m_catalog_only.clear();

    auto& cs = phos::color::GetColorSystem();
    // Stable builtin ordering.
    const BuiltinPalette order[] = {
        BuiltinPalette::Vga8,
        BuiltinPalette::Vga16,
        BuiltinPalette::Xterm16,
        BuiltinPalette::Xterm240Safe,
        BuiltinPalette::Xterm256,
    };
    for (BuiltinPalette b : order)
        m_ui_list.push_back(cs.Palettes().Builtin(b));
}

void PaletteCatalog::AppendCatalogPalette(PaletteInstanceId id)
{
    m_ui_list.push_back(id);
    m_catalog_only.push_back(id);
}

bool PaletteCatalog::LoadFromJsonFile(const std::string& path, std::string& out_error)
{
    using nlohmann::json;

    out_error.clear();
    m_last_error.clear();
    RebuildBuiltinList();

    std::ifstream f(path);
    if (!f)
    {
        // Optional file: treat as non-fatal. Builtins remain available.
        out_error = std::string("Failed to open ") + path;
        m_last_error = out_error;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        m_last_error = out_error;
        return false;
    }

    if (!j.is_array())
    {
        out_error = "Expected top-level JSON array in color-palettes.json";
        m_last_error = out_error;
        return false;
    }

    auto& cs = phos::color::GetColorSystem();
    const Palette* vga8 = cs.Palettes().Get(cs.Palettes().Builtin(BuiltinPalette::Vga8));
    const Palette* vga16 = cs.Palettes().Get(cs.Palettes().Builtin(BuiltinPalette::Vga16));
    const Palette* x16 = cs.Palettes().Get(cs.Palettes().Builtin(BuiltinPalette::Xterm16));
    const Palette* x240 = cs.Palettes().Get(cs.Palettes().Builtin(BuiltinPalette::Xterm240Safe));
    const Palette* x256 = cs.Palettes().Get(cs.Palettes().Builtin(BuiltinPalette::Xterm256));

    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        if (!item.contains("title") || !item["title"].is_string())
            continue;
        const std::string title = item["title"].get<std::string>();

        if (!item.contains("colors") || !item["colors"].is_array())
            continue;

        std::vector<Rgb8> rgb;
        rgb.reserve(std::min<std::size_t>(item["colors"].size(), kMaxPaletteSize));
        for (const auto& c : item["colors"])
        {
            if (!c.is_string())
                continue;
            Rgb8 col;
            if (!ParseHexRgb(c.get<std::string>(), col))
                continue;
            rgb.push_back(col);
            if (rgb.size() >= kMaxPaletteSize)
                break;
        }
        if (rgb.empty())
            continue;

        // Avoid duplicating builtins in the UI when the JSON includes equivalent definitions.
        if ((vga8 && RgbEquals(rgb, vga8->rgb)) ||
            (vga16 && RgbEquals(rgb, vga16->rgb)) ||
            (x16 && RgbEquals(rgb, x16->rgb)) ||
            (x240 && RgbEquals(rgb, x240->rgb)) ||
            (x256 && RgbEquals(rgb, x256->rgb)))
        {
            continue;
        }

        const PaletteInstanceId id = cs.Palettes().RegisterDynamic(title, rgb);
        AppendCatalogPalette(id);
    }

    // Successful parse (even if it had zero valid entries).
    return true;
}

bool PaletteCatalog::AppendToJsonFile(const std::string& path,
                                      std::string_view wanted_title,
                                      std::span<const Rgb8> rgb,
                                      std::string& out_error,
                                      std::string* out_final_title)
{
    using nlohmann::json;

    out_error.clear();
    if (out_final_title)
        out_final_title->clear();

    if (path.empty())
    {
        out_error = "Invalid path";
        return false;
    }
    if (rgb.empty())
    {
        out_error = "Palette has no colors";
        return false;
    }

    json j;
    {
        std::ifstream f(path);
        if (f)
        {
            try
            {
                f >> j;
            }
            catch (const std::exception& e)
            {
                out_error = e.what();
                return false;
            }
        }
        else
        {
            // Missing file: treat as empty catalog and create it.
            j = json::array();
        }
    }

    if (!j.is_array())
    {
        out_error = "Expected top-level JSON array in color-palettes.json";
        return false;
    }

    const std::string final_title = MakeUniqueTitle(j, wanted_title);
    if (out_final_title)
        *out_final_title = final_title;

    json item;
    item["title"] = final_title;
    json colors = json::array();
    const std::size_t n = std::min<std::size_t>((std::size_t)rgb.size(), kMaxPaletteSize);
    for (std::size_t i = 0; i < n; ++i)
        colors.push_back(RgbToHexRgb(rgb[i]));
    item["colors"] = std::move(colors);
    j.push_back(std::move(item));

    try
    {
        namespace fs = std::filesystem;
        const fs::path p(path);
        fs::path tmp = p;
        tmp += ".tmp";

        {
            std::ofstream out(tmp.string());
            if (!out)
            {
                out_error = std::string("Failed to write ") + tmp.string();
                return false;
            }
            out << j.dump(2) << "\n";
        }

        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (ec)
        {
            // Best-effort cleanup; keep original file intact if rename fails.
            std::error_code ec2;
            fs::remove(tmp, ec2);
            out_error = std::string("Failed to replace ") + p.string() + ": " + ec.message();
            return false;
        }
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }

    return true;
}

std::optional<PaletteInstanceId> PaletteCatalog::FindInUiListByRef(const PaletteRef& ref) const
{
    auto& cs = phos::color::GetColorSystem();
    const auto resolved = cs.Palettes().Resolve(ref);
    if (!resolved.has_value())
        return std::nullopt;
    const PaletteInstanceId want = *resolved;
    for (PaletteInstanceId id : m_ui_list)
    {
        if (id == want)
            return id;
    }
    return std::nullopt;
}

std::optional<PaletteInstanceId> PaletteCatalog::EnsureUiIncludes(const PaletteRef& ref)
{
    auto& cs = phos::color::GetColorSystem();
    const auto resolved = cs.Palettes().Resolve(ref);
    if (!resolved.has_value())
        return std::nullopt;
    const PaletteInstanceId want = *resolved;
    for (PaletteInstanceId id : m_ui_list)
    {
        if (id == want)
            return want;
    }
    // Append unknown (but resolvable) palettes so the UI can reflect the active canvas palette.
    m_ui_list.push_back(want);
    return want;
}

} // namespace phos::color


