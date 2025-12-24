#include "core/fonts.h"

#include "core/encodings.h"
#include "core/encodings_tables_generated.h"

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
static const std::unordered_map<char32_t, std::uint8_t>& Cp437ReverseMap()
{
    static const std::unordered_map<char32_t, std::uint8_t> map = []() {
        std::unordered_map<char32_t, std::uint8_t> m;
        m.reserve(256);
        for (std::uint32_t i = 0; i < 256; ++i)
            m.emplace(phos::encodings::kCp437[i], (std::uint8_t)i);
        return m;
    }();
    return map;
}

// Built-in bitmap fonts are 1bpp, 256 glyphs, 1 byte per glyph row (8 pixels wide).
// Enforce dimensional correctness at compile time.
template <size_t N>
constexpr FontInfo MakeBitmapFont8xH(FontId id,
                                     const char* label,
                                     const char* sauce_name,
                                     const std::uint8_t (&bitmap)[N])
{
    static_assert(N > 0, "Bitmap font table must not be empty.");
    static_assert(N % 256u == 0u, "Bitmap font table must be 256 glyphs * cell_h rows.");
    constexpr int cell_h = (int)(N / 256u);
    static_assert(cell_h > 0, "Bitmap font table must have a positive cell height.");
    // NOTE: The tables are 8 pixels wide because each row is 1 byte; the 9th VGA column
    // (when desired) is a render-time duplication rule, not stored in the bitmap.
    return FontInfo{id, Kind::Bitmap1bpp, label, sauce_name, 8, cell_h, bitmap, false};
}

static const std::vector<FontInfo>& BuildRegistry()
{
    static const std::vector<FontInfo> v = {
        // Note: Unscii remains the *UI font* and default canvas font, backed by ImGui's atlas.
        {FontId::Unscii, Kind::ImGuiAtlas, "Unscii 2.1 8x16", "unscii-16-full", 0, 0, nullptr, false},

        // libansilove-derived bitmap fonts (CP437-ordered glyphs).
        // SAUCE canonical names are from references/sauce-spec.md (FontName section).
        //
        // NOTE: The bitmap tables are 8px wide (1 byte per row). We intentionally use cell_w=8
        // here (rather than DOS/VGA's sometimes-emulated 9px cell) to avoid introducing a
        // permanent blank spacer column for most glyphs (especially visible on CP437 shades).
        MakeBitmapFont8xH(FontId::Font_PC_80x25, "IBM VGA 437", "IBM VGA 437", font_pc_80x25),
        MakeBitmapFont8xH(FontId::Font_PC_80x50, "IBM VGA50 437", "IBM VGA50 437", font_pc_80x50),

        // IBM PC OEM codepage fonts (names follow the SAUCE "IBM VGA ###" convention).
        // Note: We treat these as 256-glyph bitmap fonts; higher-level encoding semantics are handled elsewhere.
        MakeBitmapFont8xH(FontId::Font_PC_Latin1, "IBM VGA 850", "IBM VGA 850", font_pc_latin1),
        MakeBitmapFont8xH(FontId::Font_PC_Latin2, "IBM VGA 852", "IBM VGA 852", font_pc_latin2),
        MakeBitmapFont8xH(FontId::Font_PC_Cyrillic, "IBM VGA 855", "IBM VGA 855", font_pc_cyrillic),
        MakeBitmapFont8xH(FontId::Font_PC_Russian, "IBM VGA 866", "IBM VGA 866", font_pc_russian),
        MakeBitmapFont8xH(FontId::Font_PC_Greek, "IBM VGA 737", "IBM VGA 737", font_pc_greek),
        MakeBitmapFont8xH(FontId::Font_PC_Greek869, "IBM VGA 869", "IBM VGA 869", font_pc_greek_869),
        MakeBitmapFont8xH(FontId::Font_PC_Turkish, "IBM VGA 857", "IBM VGA 857", font_pc_turkish),
        MakeBitmapFont8xH(FontId::Font_PC_Hebrew, "IBM VGA 862", "IBM VGA 862", font_pc_hebrew),
        MakeBitmapFont8xH(FontId::Font_PC_Icelandic, "IBM VGA 861", "IBM VGA 861", font_pc_icelandic),
        MakeBitmapFont8xH(FontId::Font_PC_Nordic, "IBM VGA 865", "IBM VGA 865", font_pc_nordic),
        MakeBitmapFont8xH(FontId::Font_PC_Portuguese, "IBM VGA 860", "IBM VGA 860", font_pc_portuguese),
        MakeBitmapFont8xH(FontId::Font_PC_FrenchCanadian, "IBM VGA 863", "IBM VGA 863", font_pc_french_canadian),
        MakeBitmapFont8xH(FontId::Font_PC_Baltic, "IBM VGA 775", "IBM VGA 775", font_pc_baltic),

        // Extra bitmap fonts: these aren't in the SAUCE canonical list, but we still provide a stable hint.
        MakeBitmapFont8xH(FontId::Font_Terminus, "Terminus", "Terminus", font_pc_terminus),
        MakeBitmapFont8xH(FontId::Font_Spleen, "Spleen", "Spleen", font_pc_spleen),

        // Amiga fonts (SAUCE canonical names).
        MakeBitmapFont8xH(FontId::Font_Amiga_Topaz500, "Amiga Topaz 1", "Amiga Topaz 1", font_amiga_topaz_500),
        MakeBitmapFont8xH(FontId::Font_Amiga_Topaz500Plus, "Amiga Topaz 1+", "Amiga Topaz 1+", font_amiga_topaz_500_plus),
        MakeBitmapFont8xH(FontId::Font_Amiga_Topaz1200, "Amiga Topaz 2", "Amiga Topaz 2", font_amiga_topaz_1200),
        MakeBitmapFont8xH(FontId::Font_Amiga_Topaz1200Plus, "Amiga Topaz 2+", "Amiga Topaz 2+", font_amiga_topaz_1200_plus),
        MakeBitmapFont8xH(FontId::Font_Amiga_PotNoodle, "Amiga P0T-NOoDLE", "Amiga P0T-NOoDLE", font_amiga_pot_noodle),
        MakeBitmapFont8xH(FontId::Font_Amiga_Microknight, "Amiga MicroKnight", "Amiga MicroKnight", font_amiga_microknight),
        MakeBitmapFont8xH(FontId::Font_Amiga_MicroknightPlus, "Amiga MicroKnight+", "Amiga MicroKnight+", font_amiga_microknight_plus),
        MakeBitmapFont8xH(FontId::Font_Amiga_Mosoul, "Amiga mOsOul", "Amiga mOsOul", font_amiga_mosoul),
    };
    return v;
}

static phos::encodings::EncodingId EncodingForFontInternal(FontId id)
{
    using phos::encodings::EncodingId;
    switch (id)
    {
        case FontId::Font_PC_80x25: return EncodingId::Cp437;
        case FontId::Font_PC_80x50: return EncodingId::Cp437;

        case FontId::Font_PC_Latin1: return EncodingId::Cp850;
        case FontId::Font_PC_Latin2: return EncodingId::Cp852;
        case FontId::Font_PC_Cyrillic: return EncodingId::Cp855;
        case FontId::Font_PC_Russian: return EncodingId::Cp866;
        case FontId::Font_PC_Greek: return EncodingId::Cp737;
        case FontId::Font_PC_Greek869: return EncodingId::Cp869;
        case FontId::Font_PC_Turkish: return EncodingId::Cp857;
        case FontId::Font_PC_Hebrew: return EncodingId::Cp862;
        case FontId::Font_PC_Icelandic: return EncodingId::Cp861;
        case FontId::Font_PC_Nordic: return EncodingId::Cp865;
        case FontId::Font_PC_Portuguese: return EncodingId::Cp860;
        case FontId::Font_PC_FrenchCanadian: return EncodingId::Cp863;
        case FontId::Font_PC_Baltic: return EncodingId::Cp775;

        // Best-effort defaults for other 256-glyph bitmap tables.
        case FontId::Font_Terminus: return EncodingId::Cp437;
        case FontId::Font_Spleen: return EncodingId::Cp437;

        case FontId::Font_Amiga_Topaz500:
        case FontId::Font_Amiga_Topaz500Plus:
        case FontId::Font_Amiga_Topaz1200:
        case FontId::Font_Amiga_Topaz1200Plus:
        case FontId::Font_Amiga_PotNoodle:
        case FontId::Font_Amiga_Microknight:
        case FontId::Font_Amiga_MicroknightPlus:
        case FontId::Font_Amiga_Mosoul:
            return EncodingId::AmigaLatin1;

        case FontId::Unscii:
        default:
            return EncodingId::Cp437;
    }
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

phos::encodings::EncodingId EncodingForFont(FontId id)
{
    return EncodingForFontInternal(id);
}

char32_t Cp437ByteToUnicode(std::uint8_t b)
{
    return phos::encodings::kCp437[b];
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

    if (f.kind == Kind::Bitmap1bpp && f.bitmap != nullptr)
    {
        const phos::encodings::EncodingId enc = EncodingForFontInternal(font);
        std::uint8_t b = 0;
        if (!phos::encodings::UnicodeToByte(enc, cp, b))
            return false;
        out_glyph = (std::uint16_t)b; // 0..255
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


