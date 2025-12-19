#include "core/fonts.h"

#include <algorithm>
#include <unordered_map>

// IMPORTANT: the copied libansilove headers define global `const uint8_t font_*[]` arrays.
// They MUST be included in exactly one translation unit to avoid ODR violations.
#include "core/fonts/font_pc_80x25.h"
#include "core/fonts/font_pc_80x50.h"
#include "core/fonts/font_pc_latin1.h"
#include "core/fonts/font_pc_latin2.h"
#include "core/fonts/font_pc_cyrillic.h"
#include "core/fonts/font_pc_russian.h"
#include "core/fonts/font_pc_greek.h"
#include "core/fonts/font_pc_greek869.h"
#include "core/fonts/font_pc_turkish.h"
#include "core/fonts/font_pc_hebrew.h"
#include "core/fonts/font_pc_icelandic.h"
#include "core/fonts/font_pc_nordic.h"
#include "core/fonts/font_pc_portuguese.h"
#include "core/fonts/font_pc_french_canadian.h"
#include "core/fonts/font_pc_baltic.h"
#include "core/fonts/font_pc_terminus.h"
#include "core/fonts/font_pc_spleen.h"
#include "core/fonts/font_amiga_topaz_500.h"
#include "core/fonts/font_amiga_topaz_500_plus.h"
#include "core/fonts/font_amiga_topaz_1200.h"
#include "core/fonts/font_amiga_topaz_1200_plus.h"
#include "core/fonts/font_amiga_pot_noodle.h"
#include "core/fonts/font_amiga_microknight.h"
#include "core/fonts/font_amiga_microknight_plus.h"
#include "core/fonts/font_amiga_mosoul.h"

namespace fonts
{
namespace
{
// CP437 mapping table (0..255) -> Unicode codepoints.
// Kept in core/fonts as part of the "font authority" layer (bitmap fonts are CP437-ordered).
static constexpr char32_t kCp437[256] = {
    U'\u0000', U'\u263A', U'\u263B', U'\u2665', U'\u2666', U'\u2663', U'\u2660', U'\u2022',
    U'\u25D8', U'\u25CB', U'\u25D9', U'\u2642', U'\u2640', U'\u266A', U'\u266B', U'\u263C',
    U'\u25BA', U'\u25C4', U'\u2195', U'\u203C', U'\u00B6', U'\u00A7', U'\u25AC', U'\u21A8',
    U'\u2191', U'\u2193', U'\u2192', U'\u2190', U'\u221F', U'\u2194', U'\u25B2', U'\u25BC',
    U' ',      U'!',      U'"',      U'#',      U'$',      U'%',      U'&',      U'\'',
    U'(',      U')',      U'*',      U'+',      U',',      U'-',      U'.',      U'/',
    U'0',      U'1',      U'2',      U'3',      U'4',      U'5',      U'6',      U'7',
    U'8',      U'9',      U':',      U';',      U'<',      U'=',      U'>',      U'?',
    U'@',      U'A',      U'B',      U'C',      U'D',      U'E',      U'F',      U'G',
    U'H',      U'I',      U'J',      U'K',      U'L',      U'M',      U'N',      U'O',
    U'P',      U'Q',      U'R',      U'S',      U'T',      U'U',      U'V',      U'W',
    U'X',      U'Y',      U'Z',      U'[',      U'\\',     U']',      U'^',      U'_',
    U'`',      U'a',      U'b',      U'c',      U'd',      U'e',      U'f',      U'g',
    U'h',      U'i',      U'j',      U'k',      U'l',      U'm',      U'n',      U'o',
    U'p',      U'q',      U'r',      U's',      U't',      U'u',      U'v',      U'w',
    U'x',      U'y',      U'z',      U'{',      U'|',      U'}',      U'~',      U'\u2302',
    U'\u00C7', U'\u00FC', U'\u00E9', U'\u00E2', U'\u00E4', U'\u00E0', U'\u00E5', U'\u00E7',
    U'\u00EA', U'\u00EB', U'\u00E8', U'\u00EF', U'\u00EE', U'\u00EC', U'\u00C4', U'\u00C5',
    U'\u00C9', U'\u00E6', U'\u00C6', U'\u00F4', U'\u00F6', U'\u00F2', U'\u00FB', U'\u00F9',
    U'\u00FF', U'\u00D6', U'\u00DC', U'\u00A2', U'\u00A3', U'\u00A5', U'\u20A7', U'\u0192',
    U'\u00E1', U'\u00ED', U'\u00F3', U'\u00FA', U'\u00F1', U'\u00D1', U'\u00AA', U'\u00BA',
    U'\u00BF', U'\u2310', U'\u00AC', U'\u00BD', U'\u00BC', U'\u00A1', U'\u00AB', U'\u00BB',
    U'\u2591', U'\u2592', U'\u2593', U'\u2502', U'\u2524', U'\u2561', U'\u2562', U'\u2556',
    U'\u2555', U'\u2563', U'\u2551', U'\u2557', U'\u255D', U'\u255C', U'\u255B', U'\u2510',
    U'\u2514', U'\u2534', U'\u252C', U'\u251C', U'\u2500', U'\u253C', U'\u255E', U'\u255F',
    U'\u255A', U'\u2554', U'\u2569', U'\u2566', U'\u2560', U'\u2550', U'\u256C', U'\u2567',
    U'\u2568', U'\u2564', U'\u2565', U'\u2559', U'\u2558', U'\u2552', U'\u2553', U'\u256B',
    U'\u256A', U'\u2518', U'\u250C', U'\u2588', U'\u2584', U'\u258C', U'\u2590', U'\u2580',
    U'\u03B1', U'\u00DF', U'\u0393', U'\u03C0', U'\u03A3', U'\u03C3', U'\u00B5', U'\u03C4',
    U'\u03A6', U'\u0398', U'\u03A9', U'\u03B4', U'\u221E', U'\u03C6', U'\u03B5', U'\u2229',
    U'\u2261', U'\u00B1', U'\u2265', U'\u2264', U'\u2320', U'\u2321', U'\u00F7', U'\u2248',
    U'\u00B0', U'\u2219', U'\u00B7', U'\u221A', U'\u207F', U'\u00B2', U'\u25A0', U'\u00A0',
};

static const std::unordered_map<char32_t, std::uint8_t>& Cp437ReverseMap()
{
    static const std::unordered_map<char32_t, std::uint8_t> map = []() {
        std::unordered_map<char32_t, std::uint8_t> m;
        m.reserve(256);
        for (std::uint32_t i = 0; i < 256; ++i)
            m.emplace(kCp437[i], (std::uint8_t)i);
        return m;
    }();
    return map;
}

static const std::vector<FontInfo>& BuildRegistry()
{
    static const std::vector<FontInfo> v = {
        // Note: Unscii remains the *UI font* and default canvas font, backed by ImGui's atlas.
        {FontId::Unscii, Kind::ImGuiAtlas, "Unscii 8x16", "unscii-16-full", 0, 0, nullptr, false},

        // libansilove-derived bitmap fonts (CP437-ordered glyphs).
        // SAUCE canonical names are from references/sauce-spec.md (FontName section).
        {FontId::Font_PC_80x25, Kind::Bitmap1bpp, "IBM VGA 437", "IBM VGA 437", 9, 16, font_pc_80x25, true},
        {FontId::Font_PC_80x50, Kind::Bitmap1bpp, "IBM VGA50 437", "IBM VGA50 437", 9, 8, font_pc_80x50, true},

        // IBM PC OEM codepage fonts (names follow the SAUCE "IBM VGA ###" convention).
        // Note: We treat these as 256-glyph bitmap fonts; higher-level encoding semantics are handled elsewhere.
        {FontId::Font_PC_Latin1, Kind::Bitmap1bpp, "IBM VGA 850", "IBM VGA 850", 9, 16, font_pc_latin1, true},
        {FontId::Font_PC_Latin2, Kind::Bitmap1bpp, "IBM VGA 852", "IBM VGA 852", 9, 16, font_pc_latin2, true},
        {FontId::Font_PC_Cyrillic, Kind::Bitmap1bpp, "IBM VGA 855", "IBM VGA 855", 9, 16, font_pc_cyrillic, true},
        {FontId::Font_PC_Russian, Kind::Bitmap1bpp, "IBM VGA 866", "IBM VGA 866", 9, 16, font_pc_russian, true},
        {FontId::Font_PC_Greek, Kind::Bitmap1bpp, "IBM VGA 737", "IBM VGA 737", 9, 16, font_pc_greek, true},
        {FontId::Font_PC_Greek869, Kind::Bitmap1bpp, "IBM VGA 869", "IBM VGA 869", 9, 16, font_pc_greek_869, true},
        {FontId::Font_PC_Turkish, Kind::Bitmap1bpp, "IBM VGA 857", "IBM VGA 857", 9, 16, font_pc_turkish, true},
        {FontId::Font_PC_Hebrew, Kind::Bitmap1bpp, "IBM VGA 862", "IBM VGA 862", 9, 16, font_pc_hebrew, true},
        {FontId::Font_PC_Icelandic, Kind::Bitmap1bpp, "IBM VGA 861", "IBM VGA 861", 9, 16, font_pc_icelandic, true},
        {FontId::Font_PC_Nordic, Kind::Bitmap1bpp, "IBM VGA 865", "IBM VGA 865", 9, 16, font_pc_nordic, true},
        {FontId::Font_PC_Portuguese, Kind::Bitmap1bpp, "IBM VGA 860", "IBM VGA 860", 9, 16, font_pc_portuguese, true},
        {FontId::Font_PC_FrenchCanadian, Kind::Bitmap1bpp, "IBM VGA 863", "IBM VGA 863", 9, 16, font_pc_french_canadian, true},
        {FontId::Font_PC_Baltic, Kind::Bitmap1bpp, "IBM VGA 775", "IBM VGA 775", 9, 16, font_pc_baltic, true},

        // Extra bitmap fonts: these aren't in the SAUCE canonical list, but we still provide a stable hint.
        {FontId::Font_Terminus, Kind::Bitmap1bpp, "Terminus", "Terminus", 9, 16, font_pc_terminus, true},
        {FontId::Font_Spleen, Kind::Bitmap1bpp, "Spleen", "Spleen", 9, 16, font_pc_spleen, true},

        // Amiga fonts (SAUCE canonical names).
        {FontId::Font_Amiga_Topaz500, Kind::Bitmap1bpp, "Amiga Topaz 1", "Amiga Topaz 1", 8, 16, font_amiga_topaz_500, false},
        {FontId::Font_Amiga_Topaz500Plus, Kind::Bitmap1bpp, "Amiga Topaz 1+", "Amiga Topaz 1+", 8, 16, font_amiga_topaz_500_plus, false},
        {FontId::Font_Amiga_Topaz1200, Kind::Bitmap1bpp, "Amiga Topaz 2", "Amiga Topaz 2", 8, 16, font_amiga_topaz_1200, false},
        {FontId::Font_Amiga_Topaz1200Plus, Kind::Bitmap1bpp, "Amiga Topaz 2+", "Amiga Topaz 2+", 8, 16, font_amiga_topaz_1200_plus, false},
        {FontId::Font_Amiga_PotNoodle, Kind::Bitmap1bpp, "Amiga P0T-NOoDLE", "Amiga P0T-NOoDLE", 8, 16, font_amiga_pot_noodle, false},
        {FontId::Font_Amiga_Microknight, Kind::Bitmap1bpp, "Amiga MicroKnight", "Amiga MicroKnight", 8, 16, font_amiga_microknight, false},
        {FontId::Font_Amiga_MicroknightPlus, Kind::Bitmap1bpp, "Amiga MicroKnight+", "Amiga MicroKnight+", 8, 16, font_amiga_microknight_plus, false},
        {FontId::Font_Amiga_Mosoul, Kind::Bitmap1bpp, "Amiga mOsOul", "Amiga mOsOul", 8, 16, font_amiga_mosoul, false},
    };
    return v;
}

static bool IsCp437BitmapFont(FontId id)
{
    const FontInfo& f = Get(id);
    return f.kind == Kind::Bitmap1bpp && f.bitmap != nullptr;
}
} // namespace

FontId DefaultCanvasFont()
{
    return FontId::Unscii;
}

const std::vector<FontInfo>& AllFonts()
{
    return BuildRegistry();
}

const FontInfo& Get(FontId id)
{
    const auto& v = BuildRegistry();
    for (const auto& f : v)
        if (f.id == id)
            return f;
    return v.front();
}

bool TryFromSauceName(std::string_view tinfos, FontId& out_id)
{
    out_id = DefaultCanvasFont();

    if (tinfos.empty())
        return false;

    auto trim = [](std::string_view s) -> std::string_view {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.remove_suffix(1);
        return s;
    };

    tinfos = trim(tinfos);
    if (tinfos.empty())
        return false;

    // Normalize: SAUCE fields are typically ASCII-ish; do case-insensitive compare.
    auto eq = [](std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const char ca = a[i];
            const char cb = b[i];
            const char la = (ca >= 'A' && ca <= 'Z') ? (char)(ca - 'A' + 'a') : ca;
            const char lb = (cb >= 'A' && cb <= 'Z') ? (char)(cb - 'A' + 'a') : cb;
            if (la != lb)
                return false;
        }
        return true;
    };

    auto starts_with = [&](std::string_view s, std::string_view pref) -> bool {
        if (s.size() < pref.size())
            return false;
        return eq(s.substr(0, pref.size()), pref);
    };

    auto parse_trailing_int = [&](std::string_view s, int& out) -> bool {
        s = trim(s);
        if (s.empty())
            return false;
        int v = 0;
        bool any = false;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            any = true;
            v = v * 10 + (c - '0');
        }
        if (!any)
            return false;
        out = v;
        return true;
    };

    auto from_ibm_vga_codepage = [&](int cp) -> FontId {
        switch (cp)
        {
            case 437: return FontId::Font_PC_80x25;
            case 775: return FontId::Font_PC_Baltic;
            case 850: return FontId::Font_PC_Latin1;
            case 852: return FontId::Font_PC_Latin2;
            case 855: return FontId::Font_PC_Cyrillic;
            case 857: return FontId::Font_PC_Turkish;
            case 860: return FontId::Font_PC_Portuguese;
            case 861: return FontId::Font_PC_Icelandic;
            case 862: return FontId::Font_PC_Hebrew;
            case 863: return FontId::Font_PC_FrenchCanadian;
            case 865: return FontId::Font_PC_Nordic;
            case 866: return FontId::Font_PC_Russian;
            case 737: return FontId::Font_PC_Greek;
            case 869: return FontId::Font_PC_Greek869;
            default:  return DefaultCanvasFont();
        }
    };

    // Canonical SAUCE "FontName" parsing (references/sauce-spec.md).
    // Examples:
    // - "IBM VGA 437"
    // - "IBM VGA50 437"
    // - "Amiga Topaz 2+"
    if (starts_with(tinfos, "IBM VGA50"))
    {
        // Try to parse the codepage after the prefix (optionally separated by space).
        std::string_view rest = tinfos.substr(std::string_view("IBM VGA50").size());
        rest = trim(rest);
        if (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        rest = trim(rest);
        int cp = 0;
        if (parse_trailing_int(rest, cp))
        {
            // We only ship CP437 for 80x50 currently; fall back to the matching 80x25 font.
            if (cp == 437)
            {
                out_id = FontId::Font_PC_80x50;
                return true;
            }
            const FontId id = from_ibm_vga_codepage(cp);
            out_id = (id == DefaultCanvasFont()) ? FontId::Font_PC_80x50 : id;
            return true;
        }
        // Common shorthand seen in the wild: "IBM VGA50" with no codepage => assume 437.
        out_id = FontId::Font_PC_80x50;
        return true;
    }
    if (starts_with(tinfos, "IBM VGA"))
    {
        std::string_view rest = tinfos.substr(std::string_view("IBM VGA").size());
        rest = trim(rest);
        if (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        rest = trim(rest);
        int cp = 0;
        if (parse_trailing_int(rest, cp))
        {
            const FontId id = from_ibm_vga_codepage(cp);
            if (id != DefaultCanvasFont())
            {
                out_id = id;
                return true;
            }
            // Unknown codepage: prefer CP437 as a safe default.
            out_id = FontId::Font_PC_80x25;
            return true;
        }
        // Common shorthand seen in the wild: "IBM VGA" with no codepage => assume 437.
        out_id = FontId::Font_PC_80x25;
        return true;
    }

    // Exact match against our registry canonical names.
    for (const auto& f : AllFonts())
    {
        if (f.sauce_name && *f.sauce_name && eq(tinfos, f.sauce_name))
        {
            out_id = f.id;
            return true;
        }
    }

    // Back-compat / common aliases:
    // - older phosphor builds used short internal ids
    // - some tools write "cp437" style tags
    if (eq(tinfos, "unscii") || eq(tinfos, "unscii-16-full"))
    {
        out_id = FontId::Unscii;
        return true;
    }
    if (eq(tinfos, "cp437") || eq(tinfos, "dos") || eq(tinfos, "ibm"))
    {
        out_id = FontId::Font_PC_80x25;
        return true;
    }
    if (eq(tinfos, "cp437-80x50") || eq(tinfos, "80x50") || eq(tinfos, "vga50"))
    {
        out_id = FontId::Font_PC_80x50;
        return true;
    }
    if (eq(tinfos, "terminus"))
    {
        out_id = FontId::Font_Terminus;
        return true;
    }
    if (eq(tinfos, "spleen"))
    {
        out_id = FontId::Font_Spleen;
        return true;
    }
    if (eq(tinfos, "topaz") || eq(tinfos, "topaz1200"))
    {
        out_id = FontId::Font_Amiga_Topaz1200;
        return true;
    }
    if (eq(tinfos, "microknight"))
    {
        out_id = FontId::Font_Amiga_Microknight;
        return true;
    }
    if (eq(tinfos, "microknight+"))
    {
        out_id = FontId::Font_Amiga_MicroknightPlus;
        return true;
    }

    return false;
}

FontId FromSauceName(std::string_view tinfos)
{
    FontId id = DefaultCanvasFont();
    if (TryFromSauceName(tinfos, id))
        return id;

    // Heuristic: common SAUCE TInfoS values for scene ANSI often reference CP437.
    if (tinfos.find("cp437") != std::string_view::npos)
        return FontId::Font_PC_80x25;

    return DefaultCanvasFont();
}

std::string_view ToSauceName(FontId id)
{
    const FontInfo& f = Get(id);
    return (f.sauce_name && *f.sauce_name) ? std::string_view(f.sauce_name) : std::string_view();
}

char32_t Cp437ByteToUnicode(std::uint8_t b)
{
    return kCp437[b];
}

bool UnicodeToCp437Byte(char32_t cp, std::uint8_t& out_b)
{
    const auto& map = Cp437ReverseMap();
    auto it = map.find(cp);
    if (it == map.end())
        return false;
    out_b = it->second;
    return true;
}

bool UnicodeToGlyphIndex(FontId font, char32_t cp, std::uint16_t& out_glyph)
{
    out_glyph = 0;

    const FontInfo& f = Get(font);
    if (f.kind == Kind::ImGuiAtlas)
    {
        // Not meaningful for ImGui atlas fonts: the glyph index space is internal to ImGui,
        // and Unicode codepoints are not limited to 16-bit.
        return false;
    }

    if (IsCp437BitmapFont(font))
    {
        std::uint8_t b = 0;
        if (!UnicodeToCp437Byte(cp, b))
            return false;
        out_glyph = (std::uint16_t)b;
        return true;
    }

    return false;
}

std::uint8_t BitmapGlyphRowBits(FontId font, std::uint16_t glyph_index, int row_y)
{
    const FontInfo& f = Get(font);
    if (f.kind != Kind::Bitmap1bpp || !f.bitmap)
        return 0;
    if (glyph_index >= 256)
        return 0;
    if (row_y < 0 || row_y >= f.cell_h)
        return 0;
    const size_t off = (size_t)glyph_index * (size_t)f.cell_h + (size_t)row_y;
    return f.bitmap[off];
}
} // namespace fonts


