#include "fonts/textmode_font.h"

#include "core/fonts.h"
#include "core/xterm256_palette.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace textmode_font
{
namespace
{
// ---------------------------------------------------------------------------
// UTF-8 decoding (best-effort; matches the style used by formats::plaintext)
// ---------------------------------------------------------------------------
static void DecodeUtf8BestEffort(std::string_view bytes, std::vector<char32_t>& out)
{
    out.clear();
    size_t i = 0;
    if (bytes.size() >= 3 &&
        (std::uint8_t)bytes[0] == 0xEF &&
        (std::uint8_t)bytes[1] == 0xBB &&
        (std::uint8_t)bytes[2] == 0xBF)
        i = 3;

    const size_t len = bytes.size();
    while (i < len)
    {
        const std::uint8_t c = (std::uint8_t)bytes[i];
        char32_t cp = 0;
        size_t remaining = 0;
        if ((c & 0x80) == 0)
        {
            cp = (char32_t)c;
            remaining = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = (char32_t)(c & 0x1F);
            remaining = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = (char32_t)(c & 0x0F);
            remaining = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = (char32_t)(c & 0x07);
            remaining = 3;
        }
        else
        {
            ++i;
            continue;
        }

        if (i + remaining >= len)
            break;

        bool malformed = false;
        for (size_t j = 0; j < remaining; ++j)
        {
            const std::uint8_t cc = (std::uint8_t)bytes[i + 1 + j];
            if ((cc & 0xC0) != 0x80)
            {
                malformed = true;
                break;
            }
            cp = (cp << 6) | (char32_t)(cc & 0x3F);
        }
        if (malformed)
        {
            ++i;
            continue;
        }

        i += 1 + remaining;
        out.push_back(cp);
    }
}

// ---------------------------------------------------------------------------
// Outline style table (ported from the reference implementation)
// 19 styles, 17 glyphs each.
// ---------------------------------------------------------------------------
static constexpr std::array<std::array<char32_t, 17>, 19> kOutlineCharSetUnicode = {{
    {{U'─', U'─', U'│', U'│', U'┌', U'┐', U'┌', U'┐', U'└', U'┘', U'└', U'┘', U'┤', U'├', U' ', U' ', U' '}},
    {{U'═', U'─', U'│', U'│', U'╒', U'╕', U'┌', U'┐', U'╘', U'╛', U'└', U'┘', U'╡', U'├', U' ', U' ', U' '}},
    {{U'─', U'═', U'│', U'│', U'┌', U'┐', U'╒', U'╕', U'└', U'┘', U'╘', U'╛', U'┤', U'╞', U' ', U' ', U' '}},
    {{U'═', U'═', U'│', U'│', U'╒', U'╕', U'╒', U'╕', U'╘', U'╛', U'╘', U'╛', U'╡', U'╞', U' ', U' ', U' '}},
    {{U'─', U'─', U'║', U'│', U'╓', U'┐', U'┌', U'╖', U'└', U'╜', U'╙', U'┘', U'╢', U'├', U' ', U' ', U' '}},
    {{U'═', U'─', U'║', U'│', U'╔', U'╕', U'┌', U'╖', U'╘', U'╝', U'╙', U'┘', U'╣', U'├', U' ', U' ', U' '}},
    {{U'─', U'═', U'║', U'│', U'╓', U'┐', U'╒', U'╗', U'└', U'╜', U'╚', U'╛', U'╢', U'╞', U' ', U' ', U' '}},
    {{U'═', U'═', U'║', U'│', U'╔', U'╕', U'╒', U'╗', U'╘', U'╝', U'╚', U'╛', U'╣', U'╞', U' ', U' ', U' '}},
    {{U'─', U'─', U'│', U'║', U'┌', U'╖', U'╓', U'┐', U'╙', U'┘', U'└', U'╜', U'┤', U'╟', U' ', U' ', U' '}},
    {{U'═', U'─', U'│', U'║', U'╒', U'╗', U'╓', U'┐', U'╚', U'╛', U'└', U'╜', U'╡', U'╟', U' ', U' ', U' '}},
    {{U'─', U'═', U'│', U'║', U'┌', U'╖', U'╔', U'╕', U'╙', U'┘', U'╘', U'╝', U'┤', U'╠', U' ', U' ', U' '}},
    {{U'═', U'═', U'│', U'║', U'╒', U'╗', U'╔', U'╕', U'╚', U'╛', U'╘', U'╝', U'╡', U'╠', U' ', U' ', U' '}},
    {{U'─', U'─', U'║', U'║', U'╓', U'╖', U'╓', U'╖', U'╙', U'╜', U'╙', U'╜', U'╢', U'╟', U' ', U' ', U' '}},
    {{U'═', U'─', U'║', U'║', U'╔', U'╗', U'╓', U'╖', U'╚', U'╝', U'╙', U'╜', U'╣', U'╟', U' ', U' ', U' '}},
    {{U'─', U'═', U'║', U'║', U'╓', U'╖', U'╔', U'╗', U'╙', U'╜', U'╚', U'╝', U'╢', U'╠', U' ', U' ', U' '}},
    {{U'═', U'═', U'║', U'║', U'╔', U'╗', U'╔', U'╗', U'╚', U'╝', U'╚', U'╝', U'╣', U'╠', U' ', U' ', U' '}},
    {{U'▄', U'▄', U'█', U'█', U'▄', U'▄', U'▄', U'▄', U'█', U'█', U'█', U'█', U'█', U'█', U' ', U' ', U' '}},
    {{U'▀', U'▀', U'█', U'█', U'█', U'█', U'█', U'█', U'▀', U'▀', U'▀', U'▀', U'█', U'█', U' ', U' ', U' '}},
    {{U'▀', U'▄', U'▐', U'▌', U'▐', U'▌', U'▄', U'▄', U'▀', U'▀', U'▐', U'▌', U'█', U'█', U' ', U' ', U' '}},
}};

static char32_t TransformOutline(int outline_style, std::uint8_t placeholder)
{
    // Rust behavior:
    // if ch > 64 && ch - 64 <= 17 then:
    //  - style out of range => CP437_TO_UNICODE[ch]
    //  - else => style table [ch-65]
    // else => ' '
    if (placeholder > 64 && (int)(placeholder - 64) <= 17)
    {
        if (outline_style < 0 || outline_style >= (int)kOutlineCharSetUnicode.size())
            return fonts::Cp437ByteToUnicode(placeholder);
        return kOutlineCharSetUnicode[(size_t)outline_style][(size_t)(placeholder - 65)];
    }
    return U' ';
}

// ---------------------------------------------------------------------------
// Glyph IR (ported from the reference implementation; simplified for C++)
// ---------------------------------------------------------------------------
enum class PartKind : std::uint8_t
{
    NewLine = 0,
    EndMarker,
    HardBlank,
    FillMarker,
    OutlineHole,
    OutlinePlaceholder,
    Char,
    AnsiChar,
};

struct GlyphPart
{
    PartKind kind = PartKind::Char;
    char32_t ch = U' '; // for Char and AnsiChar
    std::uint8_t fg = 0;
    std::uint8_t bg = 0;
    bool blink = false;
    std::uint8_t placeholder = 0; // raw byte for OutlinePlaceholder

    static GlyphPart NewLine() { return GlyphPart{PartKind::NewLine}; }
    static GlyphPart EndMarker() { return GlyphPart{PartKind::EndMarker}; }
    static GlyphPart HardBlank() { return GlyphPart{PartKind::HardBlank}; }
    static GlyphPart FillMarker() { return GlyphPart{PartKind::FillMarker}; }
    static GlyphPart OutlineHole() { return GlyphPart{PartKind::OutlineHole}; }
    static GlyphPart OutlinePlaceholder(std::uint8_t b)
    {
        GlyphPart p;
        p.kind = PartKind::OutlinePlaceholder;
        p.placeholder = b;
        return p;
    }
    static GlyphPart Char(char32_t c)
    {
        GlyphPart p;
        p.kind = PartKind::Char;
        p.ch = c;
        return p;
    }
    static GlyphPart AnsiChar(char32_t c, std::uint8_t f, std::uint8_t b, bool bl)
    {
        GlyphPart p;
        p.kind = PartKind::AnsiChar;
        p.ch = c;
        p.fg = f;
        p.bg = b;
        p.blink = bl;
        return p;
    }
};

struct Glyph
{
    int width = 0;
    int height = 0;
    std::vector<GlyphPart> parts;
};

// ---------------------------------------------------------------------------
// FIGlet font (lazy glyph decoding)
// ---------------------------------------------------------------------------
struct FigletFont
{
    std::string name;
    std::string header;
    std::vector<std::string> comments;
    char32_t hard_blank = U'$';

    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<size_t, size_t>> line_ranges; // [start,end) without CR/LF
    std::vector<std::pair<size_t, size_t>> glyph_lines; // line ranges for all glyph lines in parse order
    std::array<std::uint32_t, 256> glyph_line_start{};  // start index into glyph_lines or UINT32_MAX
    std::array<std::uint8_t, 256> glyph_line_len{};     // number of lines for glyph
    std::array<std::optional<Glyph>, 256> cache{};

    std::optional<int> avg_width; // byte-width hint (used for space fallback)
    int height = 0;
};

static void ComputeLineRanges(const std::vector<std::uint8_t>& bytes,
                              std::vector<std::pair<size_t, size_t>>& out_ranges)
{
    out_ranges.clear();
    size_t start = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (bytes[i] == (std::uint8_t)'\n')
        {
            size_t end = i;
            if (end > start && bytes[end - 1] == (std::uint8_t)'\r')
                --end;
            out_ranges.emplace_back(start, end);
            start = i + 1;
        }
    }
    if (start <= bytes.size())
    {
        size_t end = bytes.size();
        if (end > start && bytes[end - 1] == (std::uint8_t)'\r')
            --end;
        if (start != end)
            out_ranges.emplace_back(start, end);
    }
}

static bool ReadFigletCharacterRanges(const FigletFont& font,
                                      size_t& io_line_idx,
                                      std::vector<std::pair<size_t, size_t>>& out_ranges,
                                      std::string& err)
{
    err.clear();
    out_ranges.clear();
    out_ranges.reserve((size_t)std::max(0, font.height));

    for (int row = 0; row < font.height; ++row)
    {
        if (io_line_idx >= font.line_ranges.size())
        {
            err = "FIGlet: incomplete character definition";
            return false;
        }
        const auto [s, e] = font.line_ranges[io_line_idx++];
        if (e < s)
        {
            err = "FIGlet: invalid line range";
            return false;
        }

        const size_t len = e - s;
        if (len >= 2 &&
            font.bytes[e - 2] == (std::uint8_t)'@' &&
            font.bytes[e - 1] == (std::uint8_t)'@')
        {
            out_ranges.emplace_back(s, e - 2);
            break;
        }
        if (len >= 1 && font.bytes[e - 1] == (std::uint8_t)'@')
        {
            out_ranges.emplace_back(s, e - 1);
            continue;
        }

        err = "FIGlet: character line missing @ marker";
        return false;
    }

    return true;
}

static Glyph DecodeFigletGlyph(FigletFont& font, int idx)
{
    Glyph g;
    g.width = 0;
    g.height = 0;

    if (idx < 0 || idx > 255)
        return g;
    const std::uint32_t start = font.glyph_line_start[(size_t)idx];
    if (start == UINT32_MAX)
        return g;

    const int len = (int)font.glyph_line_len[(size_t)idx];
    g.height = std::max(0, len);

    std::vector<char32_t> cps;
    cps.reserve(256);

    for (int row = 0; row < len; ++row)
    {
        if (row > 0)
            g.parts.push_back(GlyphPart::NewLine());

        const auto [s, e] = font.glyph_lines[(size_t)start + (size_t)row];
        std::string_view sv((const char*)font.bytes.data() + s, e - s);
        DecodeUtf8BestEffort(sv, cps);

        int line_width = 0;
        for (char32_t cp : cps)
        {
            if (cp == font.hard_blank)
                g.parts.push_back(GlyphPart::HardBlank());
            else
                g.parts.push_back(GlyphPart::Char(cp));
            ++line_width;
        }
        g.width = std::max(g.width, line_width);
    }

    return g;
}

static const Glyph* FigletGlyph(FigletFont& font, char32_t ch)
{
    if (ch > 255)
        return nullptr;
    const int idx = (int)ch;
    if (font.glyph_line_start[(size_t)idx] == UINT32_MAX)
        return nullptr;
    if (!font.cache[(size_t)idx].has_value())
        font.cache[(size_t)idx] = DecodeFigletGlyph(font, idx);
    return &(*font.cache[(size_t)idx]);
}

// ---------------------------------------------------------------------------
// TDF font (lazy glyph decoding)
// ---------------------------------------------------------------------------
static constexpr std::uint32_t kTdfFontIndicator = 0xFF00AA55u;
static constexpr std::uint16_t kTdfInvalidGlyph = 0xFFFFu;
static constexpr int kTdfCharTableSize = 94; // '!'..'~'

static inline bool ReadU16LE(const std::vector<std::uint8_t>& b, size_t off, std::uint16_t& out)
{
    if (off + 2 > b.size())
        return false;
    out = (std::uint16_t)b[off] | ((std::uint16_t)b[off + 1] << 8);
    return true;
}

static inline bool ReadU32LE(const std::vector<std::uint8_t>& b, size_t off, std::uint32_t& out)
{
    if (off + 4 > b.size())
        return false;
    out = (std::uint32_t)b[off] |
          ((std::uint32_t)b[off + 1] << 8) |
          ((std::uint32_t)b[off + 2] << 16) |
          ((std::uint32_t)b[off + 3] << 24);
    return true;
}

static inline std::optional<int> TdfIndex(char32_t ch)
{
    if (ch > 0xFF)
        return std::nullopt;
    const std::uint8_t b = (std::uint8_t)ch;
    if (b < (std::uint8_t)'!' || b > (std::uint8_t)'~')
        return std::nullopt;
    return (int)(b - (std::uint8_t)'!');
}

struct TdfFont
{
    std::string name;
    TdfFontType font_type = TdfFontType::Block;
    int spacing = 1;

    std::vector<std::uint8_t> bytes;
    size_t glyph_block_base = 0;
    size_t glyph_block_end = 0;
    std::array<std::uint16_t, kTdfCharTableSize> lookup{};
    std::array<std::optional<Glyph>, kTdfCharTableSize> cache{};
};

static Glyph DecodeTdfGlyph(const TdfFont& font, int idx)
{
    Glyph g;
    g.width = 0;
    g.height = 0;
    g.parts.clear();

    if (idx < 0 || idx >= kTdfCharTableSize)
        return g;
    const std::uint16_t off16 = font.lookup[(size_t)idx];
    if (off16 == kTdfInvalidGlyph)
        return g;

    const size_t off = (size_t)off16;
    size_t p = font.glyph_block_base + off;
    if (p + 2 > font.glyph_block_end || p + 2 > font.bytes.size())
        return g;

    const std::uint8_t w = font.bytes[p + 0];
    const std::uint8_t h = font.bytes[p + 1];
    g.width = (int)w;
    g.height = (int)h;
    p += 2;

    g.parts.reserve((size_t)std::max(1, g.width * g.height));

    while (p < font.glyph_block_end && p < font.bytes.size())
    {
        const std::uint8_t chb = font.bytes[p++];
        if (chb == 0)
            break;
        if (chb == 13)
        {
            g.parts.push_back(GlyphPart::NewLine());
            continue;
        }
        if (chb == (std::uint8_t)'&')
        {
            g.parts.push_back(GlyphPart::EndMarker());
            continue;
        }

        switch (font.font_type)
        {
            case TdfFontType::Color:
            {
                if (p >= font.glyph_block_end || p >= font.bytes.size())
                    break;
                const std::uint8_t attr = font.bytes[p++];
                const std::uint8_t fg = (std::uint8_t)(attr & 0x0Fu);
                const std::uint8_t bg = (std::uint8_t)((attr >> 4) & 0x07u);
                const bool blink = (attr & 0x80u) != 0;
                if (chb == 0xFF)
                {
                    g.parts.push_back(GlyphPart::HardBlank());
                }
                else
                {
                    const char32_t uc = fonts::Cp437ByteToUnicode(chb);
                    g.parts.push_back(GlyphPart::AnsiChar(uc, fg, bg, blink));
                }
                break;
            }
            case TdfFontType::Block:
            {
                if (chb == 0xFF)
                    g.parts.push_back(GlyphPart::HardBlank());
                else
                    g.parts.push_back(GlyphPart::Char(fonts::Cp437ByteToUnicode(chb)));
                break;
            }
            case TdfFontType::Outline:
            default:
            {
                if (chb == (std::uint8_t)'@')
                {
                    g.parts.push_back(GlyphPart::FillMarker());
                }
                else if (chb == (std::uint8_t)'O')
                {
                    g.parts.push_back(GlyphPart::OutlineHole());
                }
                else if (chb >= (std::uint8_t)'A' && chb <= (std::uint8_t)'R')
                {
                    g.parts.push_back(GlyphPart::OutlinePlaceholder(chb));
                }
                else if (chb == (std::uint8_t)' ')
                {
                    g.parts.push_back(GlyphPart::Char(U' '));
                }
                else
                {
                    g.parts.push_back(GlyphPart::Char(fonts::Cp437ByteToUnicode(chb)));
                }
                break;
            }
        }
    }

    return g;
}

static const Glyph* TdfGlyph(TdfFont& font, char32_t ch)
{
    const auto idx = TdfIndex(ch);
    if (!idx.has_value())
        return nullptr;
    const int i = *idx;
    if (font.lookup[(size_t)i] == kTdfInvalidGlyph)
        return nullptr;
    if (!font.cache[(size_t)i].has_value())
        font.cache[(size_t)i] = DecodeTdfGlyph(font, i);
    return &(*font.cache[(size_t)i]);
}

// ---------------------------------------------------------------------------
// Internal pimpl payload
// ---------------------------------------------------------------------------
struct FontImpl
{
    Kind kind = Kind::Figlet;
    FigletFont figlet;
    TdfFont tdf;
};

// ---------------------------------------------------------------------------
// Glyph rendering to a bitmap slice (ported from Glyph::render in Rust)
// ---------------------------------------------------------------------------
struct TmpCell
{
    char32_t cp = U' ';
    std::uint32_t fg = 0;
    std::uint32_t bg = 0;
};

static void RenderGlyphToCells(const Glyph& g,
                              const RenderOptions& opt,
                              int tdf_color_default_fg,
                              int tdf_color_default_bg,
                              std::vector<TmpCell>& out,
                              int& out_w,
                              int& out_h)
{
    out_w = std::max(0, g.width);
    out_h = std::max(0, g.height);
    out.clear();
    if (out_w <= 0 || out_h <= 0)
        return;
    out.resize((size_t)out_w * (size_t)out_h, TmpCell{});

    int x = 0;
    int y = 0;
    auto put = [&](char32_t cp, std::optional<std::uint32_t> fg, std::optional<std::uint32_t> bg) {
        if (x >= 0 && x < out_w && y >= 0 && y < out_h)
        {
            const size_t idx = (size_t)y * (size_t)out_w + (size_t)x;
            out[idx].cp = cp;
            if (fg.has_value())
                out[idx].fg = *fg;
            if (bg.has_value())
                out[idx].bg = *bg;
        }
        ++x;
    };

    for (const auto& part : g.parts)
    {
        switch (part.kind)
        {
            case PartKind::NewLine:
                ++y;
                x = 0;
                break;
            case PartKind::EndMarker:
                if (opt.mode == RenderMode::Edit)
                    put(U'&', std::nullopt, std::nullopt);
                break;
            case PartKind::HardBlank:
                if (opt.mode == RenderMode::Edit)
                    put(fonts::Cp437ByteToUnicode(0xFF), std::nullopt, std::nullopt);
                else
                    put(U' ', std::nullopt, std::nullopt);
                break;
            case PartKind::FillMarker:
                if (opt.mode == RenderMode::Edit)
                    put(U'@', std::nullopt, std::nullopt);
                else
                    put(U' ', std::nullopt, std::nullopt);
                break;
            case PartKind::OutlineHole:
                if (opt.mode == RenderMode::Edit)
                    put(U'O', std::nullopt, std::nullopt);
                else
                    put(U' ', std::nullopt, std::nullopt);
                break;
            case PartKind::OutlinePlaceholder:
            {
                char32_t cp = U' ';
                if (opt.mode == RenderMode::Edit)
                    cp = (char32_t)part.placeholder;
                else
                    cp = TransformOutline(opt.outline_style, part.placeholder);
                put(cp, std::nullopt, std::nullopt);
                break;
            }
            case PartKind::Char:
                put(part.ch, std::nullopt, std::nullopt);
                break;
            case PartKind::AnsiChar:
            {
                // Convert DOS 16-color indices to packed colors (xterm base 0..15).
                // Blink bit can be used as bright background (ICE colors) if enabled.
                const int fg_idx = std::clamp((int)part.fg, 0, 15);
                int bg_idx = std::clamp((int)part.bg, 0, 7);
                if (part.blink && opt.icecolors)
                    bg_idx = std::clamp(bg_idx + 8, 0, 15);

                // If caller doesn't want font colors, keep them unset.
                if (!opt.use_font_colors)
                {
                    put(part.ch, std::nullopt, std::nullopt);
                }
                else
                {
                    (void)tdf_color_default_fg;
                    (void)tdf_color_default_bg;
                    put(part.ch,
                        (std::uint32_t)xterm256::Color32ForIndex(fg_idx),
                        (std::uint32_t)xterm256::Color32ForIndex(bg_idx));
                }
                break;
            }
        }
        if (y >= out_h)
            break;
    }
}

static bool IsFigletMagic(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 5 && std::memcmp(bytes.data(), "flf2a", 5) == 0;
}

static bool IsTdfMagic(const std::vector<std::uint8_t>& bytes)
{
    static constexpr std::uint8_t kIdLen = 0x13; // 19
    static constexpr char kId[] = "TheDraw FONTS file";
    if (bytes.size() < 19)
        return false;
    if (bytes[0] != kIdLen)
        return false;
    return std::memcmp(bytes.data() + 1, kId, 18) == 0;
}

static bool ParseFiglet(const std::vector<std::uint8_t>& bytes, FontImpl& out, std::string& err)
{
    err.clear();
    out.kind = Kind::Figlet;
    out.figlet = FigletFont{};
    out.figlet.name = "figlet";
    out.figlet.bytes = bytes;
    out.figlet.glyph_line_start.fill(UINT32_MAX);
    out.figlet.glyph_line_len.fill(0);

    ComputeLineRanges(out.figlet.bytes, out.figlet.line_ranges);
    if (out.figlet.line_ranges.empty())
    {
        err = "FIGlet: missing or invalid header";
        return false;
    }

    size_t line_idx = 0;
    const auto [hs, he] = out.figlet.line_ranges[line_idx++];
    std::string_view header_sv((const char*)out.figlet.bytes.data() + hs, he - hs);
    if (header_sv.size() < 5 || header_sv.substr(0, 5) != "flf2a")
    {
        err = "FIGlet: not a flf2a header";
        return false;
    }

    // hard blank is the char immediately after "flf2a"
    out.figlet.hard_blank = (header_sv.size() >= 6) ? (char32_t)(std::uint8_t)header_sv[5] : U'$';
    out.figlet.header = std::string(header_sv);

    // Split header whitespace.
    std::vector<std::string_view> parts;
    parts.reserve(8);
    size_t i = 0;
    while (i < header_sv.size())
    {
        while (i < header_sv.size() && (header_sv[i] == ' ' || header_sv[i] == '\t'))
            ++i;
        if (i >= header_sv.size())
            break;
        size_t j = i;
        while (j < header_sv.size() && header_sv[j] != ' ' && header_sv[j] != '\t')
            ++j;
        parts.push_back(header_sv.substr(i, j - i));
        i = j;
    }
    if (parts.size() < 6)
    {
        err = "FIGlet: incomplete header";
        return false;
    }

    auto parse_int = [&](std::string_view s, int& out_v) -> bool {
        out_v = 0;
        if (s.empty())
            return false;
        int sign = 1;
        size_t k = 0;
        if (s[0] == '-')
        {
            sign = -1;
            k = 1;
        }
        bool any = false;
        for (; k < s.size(); ++k)
        {
            const char c = s[k];
            if (c < '0' || c > '9')
                return false;
            any = true;
            out_v = out_v * 10 + (c - '0');
        }
        if (!any)
            return false;
        out_v *= sign;
        return true;
    };

    int height = 0;
    if (!parse_int(parts[1], height) || height <= 0)
    {
        err = "FIGlet: missing height in header";
        return false;
    }
    out.figlet.height = height;

    int comment_count = 0;
    (void)parse_int(parts[5], comment_count);
    comment_count = std::max(0, comment_count);

    for (int c = 0; c < comment_count && line_idx < out.figlet.line_ranges.size(); ++c)
    {
        const auto [cs, ce] = out.figlet.line_ranges[line_idx++];
        out.figlet.comments.emplace_back(std::string((const char*)out.figlet.bytes.data() + cs, ce - cs));
    }

    // Parse glyph ranges for required ASCII characters 32..126.
    std::vector<std::pair<size_t, size_t>> ranges;
    int sum_width = 0;
    int count = 0;
    for (int ch = 32; ch <= 126; ++ch)
    {
        if (!ReadFigletCharacterRanges(out.figlet, line_idx, ranges, err))
            break;
        const std::uint32_t start = (std::uint32_t)out.figlet.glyph_lines.size();
        int max_w = 0;
        for (const auto& r : ranges)
            max_w = std::max<int>(max_w, (int)(r.second - r.first));
        out.figlet.glyph_lines.insert(out.figlet.glyph_lines.end(), ranges.begin(), ranges.end());
        out.figlet.glyph_line_start[(size_t)ch] = start;
        out.figlet.glyph_line_len[(size_t)ch] = (std::uint8_t)(out.figlet.glyph_lines.size() - (size_t)start);
        sum_width += max_w;
        count += 1;
    }

    // Try to load one more character (often 127).
    if (ReadFigletCharacterRanges(out.figlet, line_idx, ranges, err))
    {
        const std::uint32_t start = (std::uint32_t)out.figlet.glyph_lines.size();
        int max_w = 0;
        for (const auto& r : ranges)
            max_w = std::max<int>(max_w, (int)(r.second - r.first));
        out.figlet.glyph_lines.insert(out.figlet.glyph_lines.end(), ranges.begin(), ranges.end());
        out.figlet.glyph_line_start[127] = start;
        out.figlet.glyph_line_len[127] = (std::uint8_t)(out.figlet.glyph_lines.size() - (size_t)start);
        sum_width += max_w;
        count += 1;
    }

    if (count > 0)
        out.figlet.avg_width = sum_width / count;
    else
        out.figlet.avg_width = std::nullopt;

    return true;
}

static bool ParseTdfBundle(const std::vector<std::uint8_t>& bytes, std::vector<FontImpl>& out_fonts, std::string& err)
{
    err.clear();
    out_fonts.clear();

    static constexpr std::uint8_t kIdLen = 0x13;
    static constexpr char kId[] = "TheDraw FONTS file";
    static constexpr std::uint8_t kCtrlZ = 0x1A;

    if (bytes.size() < 20)
    {
        err = "TDF: file too short";
        return false;
    }
    size_t o = 0;
    const std::uint8_t id_len = bytes[o++];
    if (id_len != kIdLen)
    {
        err = "TDF: invalid header length";
        return false;
    }
    if (o + 18 > bytes.size() || std::memcmp(bytes.data() + o, kId, 18) != 0)
    {
        err = "TDF: header ID mismatch";
        return false;
    }
    o += 18;

    // Some variants include a NUL between the header string and CTRL-Z.
    // (Matches the tolerant detection logic in our font collection tooling.)
    if (o < bytes.size() && bytes[o] == 0x00)
        o += 1;

    if (o >= bytes.size() || bytes[o] != kCtrlZ)
    {
        err = "TDF: missing CTRL-Z marker";
        return false;
    }
    o += 1;

    auto is_all_zero_from = [&](size_t start) -> bool {
        for (size_t i = start; i < bytes.size(); ++i)
            if (bytes[i] != 0)
                return false;
        return true;
    };
    auto has_sauce_trailer = [&]() -> std::optional<size_t> {
        // SAUCE metadata is commonly appended to files (128-byte record at EOF),
        // sometimes preceded by a CTRL-Z (0x1A) DOS EOF marker.
        // See: https://www.acid.org/info/sauce/sauce.htm (conceptually; we just detect signature).
        static constexpr char kSauce[] = "SAUCE00";
        if (bytes.size() < 128)
            return std::nullopt;
        const size_t pos = bytes.size() - 128;
        if (std::memcmp(bytes.data() + pos, kSauce, 7) == 0)
            return pos;
        return std::nullopt;
    };
    const std::optional<size_t> sauce_pos = has_sauce_trailer();

    while (o < bytes.size())
    {
        // If a SAUCE trailer begins here (or at the next byte, after a CTRL-Z),
        // treat it as end-of-bundle and ignore it.
        if (sauce_pos.has_value() && (o == *sauce_pos || (o + 1) == *sauce_pos))
            break;

        if (bytes[o] == 0)
            break; // bundle terminator

        std::uint32_t indicator = 0;
        if (!ReadU32LE(bytes, o, indicator))
        {
            err = "TDF: truncated data at indicator";
            return false;
        }
        if (indicator != kTdfFontIndicator)
        {
            // Tolerate a common "trailer then zero padding" variant seen in the wild:
            // after decoding at least one font record, some bundles end without the 0x00
            // terminator and include a small trailer followed by zeros.
            //
            // Example: assets/fonts/tdf/guardf2.tdf (trailer begins with 0x50 00 19 00 ... then zeros).
            if (!out_fonts.empty())
            {
                // If the remainder is just a SAUCE trailer (optionally preceded by CTRL-Z), stop.
                if (sauce_pos.has_value() && (o == *sauce_pos || (o + 1) == *sauce_pos))
                    break;

                // If everything AFTER a 4-byte trailer is zero, treat it as end-of-bundle.
                if (o + 4 <= bytes.size() && is_all_zero_from(o + 4))
                    break;
                // Or if the remainder is all zero (padding), also stop.
                if (is_all_zero_from(o))
                    break;
            }

            err = "TDF: font indicator mismatch";
            return false;
        }
        o += 4;

        if (o >= bytes.size())
        {
            err = "TDF: truncated data at name length";
            return false;
        }
        const size_t orig_len = (size_t)bytes[o++];
        (void)orig_len;
        if (o + 12 > bytes.size())
        {
            err = "TDF: truncated data at name";
            return false;
        }
        size_t name_len = std::min<size_t>(orig_len, 16);
        name_len = std::min<size_t>(name_len, 12);
        for (size_t i = 0; i < name_len; ++i)
        {
            if (bytes[o + i] == 0)
            {
                name_len = i;
                break;
            }
        }
        std::string name((const char*)bytes.data() + o, name_len);
        o += 12;
        if (o + 4 > bytes.size())
        {
            err = "TDF: truncated data at reserved bytes";
            return false;
        }
        o += 4; // magic bytes

        if (o >= bytes.size())
        {
            err = "TDF: truncated data at font type";
            return false;
        }
        TdfFontType ftype = TdfFontType::Block;
        const std::uint8_t type_b = bytes[o++];
        if (type_b == 0)
            ftype = TdfFontType::Outline;
        else if (type_b == 1)
            ftype = TdfFontType::Block;
        else if (type_b == 2)
            ftype = TdfFontType::Color;
        else
        {
            err = "TDF: unsupported font type";
            return false;
        }

        if (o >= bytes.size())
        {
            err = "TDF: truncated data at spacing";
            return false;
        }
        const int spacing = (int)bytes[o++];
        std::uint16_t block_size16 = 0;
        if (!ReadU16LE(bytes, o, block_size16))
        {
            err = "TDF: truncated data at block size";
            return false;
        }
        o += 2;
        const size_t block_size = (size_t)block_size16;

        if (o + (size_t)kTdfCharTableSize * 2 > bytes.size())
        {
            err = "TDF: truncated data at char table";
            return false;
        }
        std::array<std::uint16_t, kTdfCharTableSize> lookup{};
        for (int i = 0; i < kTdfCharTableSize; ++i)
        {
            std::uint16_t v = 0;
            (void)ReadU16LE(bytes, o, v);
            lookup[(size_t)i] = v;
            o += 2;
        }

        if (o + block_size > bytes.size())
        {
            err = "TDF: truncated data at glyph block";
            return false;
        }

        // Validate offsets once (matches Rust behavior).
        for (std::uint16_t off16 : lookup)
        {
            if (off16 == kTdfInvalidGlyph)
                continue;
            if ((size_t)off16 >= block_size)
            {
                err = "TDF: glyph offset exceeds block size";
                return false;
            }
        }

        FontImpl f;
        f.kind = Kind::Tdf;
        f.tdf = TdfFont{};
        f.tdf.name = name.empty() ? "tdf" : name;
        f.tdf.font_type = ftype;
        f.tdf.spacing = spacing;
        f.tdf.lookup = lookup;
        f.tdf.bytes = bytes; // copy for now (simple, predictable)
        f.tdf.glyph_block_base = o;
        f.tdf.glyph_block_end = o + block_size;

        out_fonts.push_back(std::move(f));
        o += block_size;
    }

    if (out_fonts.empty())
    {
        err = "TDF: bundle contains no fonts";
        return false;
    }
    return true;
}

static bool HasChar(const FontImpl& font, char32_t ch)
{
    if (font.kind == Kind::Figlet)
        return FigletGlyph(const_cast<FigletFont&>(font.figlet), ch) != nullptr;
    return TdfGlyph(const_cast<TdfFont&>(font.tdf), ch) != nullptr;
}

static int SpaceFallbackWidth(const FontImpl& font)
{
    if (font.kind == Kind::Tdf)
        return std::max(1, font.tdf.spacing);
    if (font.figlet.avg_width.has_value())
        return std::max(1, *font.figlet.avg_width);
    return 1;
}

static char32_t OppositeCaseFallback(const FontImpl& font, char32_t ch)
{
    // Only best-effort for ASCII letters (typical FIGlet/TDF usage).
    if (ch >= U'a' && ch <= U'z')
    {
        char32_t up = ch - U'a' + U'A';
        if (HasChar(font, up))
            return up;
    }
    if (ch >= U'A' && ch <= U'Z')
    {
        char32_t lo = ch - U'A' + U'a';
        if (HasChar(font, lo))
            return lo;
    }
    return ch;
}

static bool RenderLine(const FontImpl& font,
                       const std::vector<char32_t>& text_cps,
                       size_t start,
                       size_t end,
                       const RenderOptions& opt,
                       std::vector<TmpCell>& out_cells,
                       int& out_w,
                       int& out_h)
{
    out_cells.clear();
    out_w = 0;
    out_h = 0;

    // Determine line height: max glyph height among chars we can render.
    int line_h = 1;
    for (size_t i = start; i < end; ++i)
    {
        const char32_t ch = text_cps[i];
        if (ch == U'\n')
            continue;
        if (ch == U'\r')
            continue;
        const Glyph* g = nullptr;
        if (font.kind == Kind::Figlet)
            g = FigletGlyph(const_cast<FigletFont&>(font.figlet), OppositeCaseFallback(font, ch));
        else
            g = TdfGlyph(const_cast<TdfFont&>(font.tdf), OppositeCaseFallback(font, ch));
        if (g && g->height > 0)
            line_h = std::max(line_h, g->height);
    }

    // Render each glyph and append horizontally.
    std::vector<TmpCell> glyph_cells;
    int gw = 0, gh = 0;

    std::vector<TmpCell> line;
    line.reserve(1024);
    int cur_w = 0;

    auto append_blank = [&](int w) {
        for (int i = 0; i < w; ++i)
        {
            for (int y = 0; y < line_h; ++y)
            {
                // We'll fill row-major later; store as column-major temp:
                // push per-row cells when we finalize.
                (void)y;
            }
        }
    };
    (void)append_blank;

    // We'll build row-major directly: for each row, keep a vector.
    std::vector<std::vector<TmpCell>> rows((size_t)line_h);
    for (auto& r : rows)
        r.clear();

    for (size_t i = start; i < end; ++i)
    {
        char32_t ch = text_cps[i];
        if (ch == U'\r')
            continue;

        // Space fallback if not defined.
        if (ch == U' ' && !HasChar(font, U' '))
        {
            const int sw = SpaceFallbackWidth(font);
            for (int y = 0; y < line_h; ++y)
                rows[(size_t)y].insert(rows[(size_t)y].end(), (size_t)sw, TmpCell{});
            cur_w += sw;
            continue;
        }

        // Case fallback.
        ch = OppositeCaseFallback(font, ch);

        const Glyph* g = nullptr;
        if (font.kind == Kind::Figlet)
            g = FigletGlyph(const_cast<FigletFont&>(font.figlet), ch);
        else
            g = TdfGlyph(const_cast<TdfFont&>(font.tdf), ch);

        if (!g || g->width <= 0 || g->height <= 0)
        {
            // Unknown char: best-effort: render '?' if present, else a single space.
            char32_t fallback = U'?';
            if (!HasChar(font, fallback))
                fallback = U' ';
            if (fallback == U' ')
            {
                for (int y = 0; y < line_h; ++y)
                    rows[(size_t)y].push_back(TmpCell{});
                cur_w += 1;
                continue;
            }
            if (font.kind == Kind::Figlet)
                g = FigletGlyph(const_cast<FigletFont&>(font.figlet), fallback);
            else
                g = TdfGlyph(const_cast<TdfFont&>(font.tdf), fallback);
            if (!g || g->width <= 0 || g->height <= 0)
            {
                for (int y = 0; y < line_h; ++y)
                    rows[(size_t)y].push_back(TmpCell{});
                cur_w += 1;
                continue;
            }
        }

        RenderGlyphToCells(*g, opt, 7, 0, glyph_cells, gw, gh);
        if (gw <= 0 || gh <= 0)
            continue;

        for (int y = 0; y < line_h; ++y)
        {
            if (y < gh)
            {
                const TmpCell* src = glyph_cells.data() + (size_t)y * (size_t)gw;
                rows[(size_t)y].insert(rows[(size_t)y].end(), src, src + gw);
            }
            else
            {
                rows[(size_t)y].insert(rows[(size_t)y].end(), (size_t)gw, TmpCell{});
            }
        }
        cur_w += gw;
    }

    out_w = cur_w;
    out_h = line_h;
    out_cells.resize((size_t)out_w * (size_t)out_h, TmpCell{});
    for (int y = 0; y < out_h; ++y)
    {
        auto& r = rows[(size_t)y];
        if ((int)r.size() < out_w)
            r.resize((size_t)out_w, TmpCell{});
        for (int x = 0; x < out_w; ++x)
            out_cells[(size_t)y * (size_t)out_w + (size_t)x] = r[(size_t)x];
    }

    return true;
}
} // namespace

// Define the PIMPL type declared in the header.
struct Font::Impl : FontImpl {};

bool LoadFontsFromBytes(const std::vector<std::uint8_t>& bytes,
                        std::vector<Font>& out_fonts,
                        std::string& err)
{
    err.clear();
    out_fonts.clear();

    if (IsFigletMagic(bytes))
    {
        FontImpl impl;
        if (!ParseFiglet(bytes, impl, err))
            return false;
        Font f;
        f.kind = Kind::Figlet;
        {
            auto p = std::make_shared<Font::Impl>();
            *static_cast<FontImpl*>(p.get()) = std::move(impl);
            f.impl = std::move(p);
        }
        out_fonts.push_back(std::move(f));
        return true;
    }
    if (IsTdfMagic(bytes))
    {
        std::vector<FontImpl> impls;
        if (!ParseTdfBundle(bytes, impls, err))
            return false;
        out_fonts.reserve(impls.size());
        for (auto& impl : impls)
        {
            Font f;
            f.kind = Kind::Tdf;
            {
                auto p = std::make_shared<Font::Impl>();
                *static_cast<FontImpl*>(p.get()) = std::move(impl);
                f.impl = std::move(p);
            }
            out_fonts.push_back(std::move(f));
        }
        return true;
    }

    err = "Unrecognized font format (expected FIGlet flf2a or TheDraw TDF).";
    return false;
}

FontMeta GetMeta(const Font& font)
{
    FontMeta m;
    m.kind = font.kind;
    const FontImpl* impl = (const FontImpl*)font.impl.get();
    if (!impl)
        return m;
    if (font.kind == Kind::Figlet)
    {
        m.name = impl->figlet.name;
        m.spacing = SpaceFallbackWidth(*impl);
        m.tdf_type = TdfFontType::Block;
    }
    else
    {
        m.name = impl->tdf.name;
        m.spacing = std::max(1, impl->tdf.spacing);
        m.tdf_type = impl->tdf.font_type;
    }
    return m;
}

bool RenderText(const Font& font,
                std::string_view utf8_text,
                const RenderOptions& options,
                Bitmap& out,
                std::string& err)
{
    err.clear();
    out = Bitmap{};

    const FontImpl* impl = (const FontImpl*)font.impl.get();
    if (!impl)
    {
        err = "Font is not initialized.";
        return false;
    }

    // Decode input text (UTF-8).
    std::vector<char32_t> cps;
    cps.reserve(utf8_text.size());
    DecodeUtf8BestEffort(utf8_text, cps);

    // Split into lines on '\n' (ignore '\r').
    std::vector<TmpCell> all_cells;
    int out_w = 0;
    int out_h = 0;

    std::vector<TmpCell> line_cells;
    int line_w = 0;
    int line_h = 0;

    std::vector<std::vector<TmpCell>> rendered_lines;
    std::vector<std::pair<int, int>> line_dims; // w,h

    size_t start = 0;
    for (size_t i = 0; i <= cps.size(); ++i)
    {
        const bool at_end = (i == cps.size());
        const bool is_nl = (!at_end && cps[i] == U'\n');
        if (!at_end && !is_nl)
            continue;

        (void)RenderLine(*impl, cps, start, i, options, line_cells, line_w, line_h);
        rendered_lines.push_back(std::move(line_cells));
        line_dims.emplace_back(line_w, line_h);
        line_cells.clear();
        line_w = 0;
        line_h = 0;

        start = i + 1;
    }

    // Compute final geometry.
    out_w = 1;
    out_h = 0;
    for (const auto& [w, h] : line_dims)
    {
        out_w = std::max(out_w, std::max(1, w));
        out_h += std::max(1, h);
    }
    if (rendered_lines.empty())
    {
        out_w = 1;
        out_h = 1;
        rendered_lines.push_back(std::vector<TmpCell>(1, TmpCell{}));
        line_dims.push_back({1, 1});
    }

    all_cells.resize((size_t)out_w * (size_t)out_h, TmpCell{});
    int yoff = 0;
    for (size_t li = 0; li < rendered_lines.size(); ++li)
    {
        const int lw = std::max(1, line_dims[li].first);
        const int lh = std::max(1, line_dims[li].second);
        const auto& lc = rendered_lines[li];
        for (int y = 0; y < lh; ++y)
        {
            for (int x = 0; x < out_w; ++x)
            {
                TmpCell c{};
                if (x < lw && (size_t)(y * lw + x) < lc.size())
                    c = lc[(size_t)y * (size_t)lw + (size_t)x];
                all_cells[(size_t)(yoff + y) * (size_t)out_w + (size_t)x] = c;
            }
        }
        yoff += lh;
    }

    out.w = out_w;
    out.h = out_h;
    out.cp.resize((size_t)out_w * (size_t)out_h, U' ');
    out.fg.resize((size_t)out_w * (size_t)out_h, 0);
    out.bg.resize((size_t)out_w * (size_t)out_h, 0);
    for (size_t i = 0; i < all_cells.size(); ++i)
    {
        out.cp[i] = all_cells[i].cp;
        out.fg[i] = all_cells[i].fg;
        out.bg[i] = all_cells[i].bg;
    }

    return true;
}
} // namespace textmode_font


