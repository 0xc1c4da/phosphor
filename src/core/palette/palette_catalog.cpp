#include "core/palette/palette_catalog.h"

#include "core/color_system.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
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


