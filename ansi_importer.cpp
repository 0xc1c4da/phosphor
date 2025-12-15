#include "ansi_importer.h"

#include "xterm256_palette.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace ansi_importer
{
namespace
{
static constexpr std::uint8_t LF  = '\n';
static constexpr std::uint8_t CR  = '\r';
static constexpr std::uint8_t TAB = '\t';
static constexpr std::uint8_t SUB = 26;
static constexpr std::uint8_t ESC = 27;

// CP437 mapping table (0..255) -> Unicode codepoints.
// Source: standard IBM Code Page 437 mapping.
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

static inline AnsiCanvas::Color32 PackImGuiCol32(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Dear ImGui IM_COL32 is ABGR.
    return 0xFF000000u | ((std::uint32_t)b << 16) | ((std::uint32_t)g << 8) | (std::uint32_t)r;
}

static std::vector<std::uint8_t> ReadAllBytes(const std::string& path, std::string& err)
{
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file for reading.";
        return {};
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0)
    {
        err = "Failed to read file size.";
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(sz));
    if (sz > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return {};
    }
    return bytes;
}

static bool DecodeOneUtf8(const std::uint8_t* data, size_t len, size_t& i, char32_t& out_cp)
{
    out_cp = U'\0';
    if (i >= len)
        return false;

    const std::uint8_t c = data[i];
    if ((c & 0x80u) == 0)
    {
        out_cp = (char32_t)c;
        i += 1;
        return true;
    }

    size_t remaining = 0;
    char32_t cp = 0;
    if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; remaining = 1; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; remaining = 2; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; remaining = 3; }
    else
    {
        i += 1;
        return false;
    }

    if (i + remaining >= len)
    {
        i = len;
        return false;
    }

    for (size_t j = 0; j < remaining; ++j)
    {
        const std::uint8_t cc = data[i + 1 + j];
        if ((cc & 0xC0u) != 0x80u)
        {
            i += 1;
            return false;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }

    i += 1 + remaining;
    out_cp = cp;
    return true;
}

static inline int ClampColumns(int columns)
{
    if (columns < 1) return 1;
    if (columns > 4096) return 4096;
    return columns;
}

static bool ContainsEsc(const std::vector<std::uint8_t>& bytes)
{
    for (std::uint8_t b : bytes)
        if (b == ESC)
            return true;
    return false;
}

static bool LooksLikeUtf8Text(const std::vector<std::uint8_t>& bytes)
{
    // Heuristic:
    // - If there are no bytes >= 0x80, there's nothing to distinguish.
    // - If there are many non-ASCII bytes and decoding succeeds with very few failures,
    //   treat as UTF-8.
    size_t non_ascii = 0;
    for (std::uint8_t b : bytes)
        if (b >= 0x80u)
            non_ascii++;
    if (non_ascii == 0)
        return false;

    size_t ok = 0;
    size_t bad = 0;
    size_t i = 0;
    while (i < bytes.size())
    {
        const std::uint8_t b = bytes[i];
        if (b < 0x80u)
        {
            i++;
            continue;
        }
        char32_t cp = U'\0';
        const size_t before = i;
        if (DecodeOneUtf8(bytes.data(), bytes.size(), i, cp))
            ok++;
        else
        {
            bad++;
            i = before + 1;
        }
    }

    // Require "strong" signal: mostly-valid multibyte sequences.
    const size_t total = ok + bad;
    if (total == 0)
        return false;
    const double ratio = (double)ok / (double)total;
    return ratio >= 0.95 && ok >= 4;
}

struct SauceInfo
{
    bool valid = false;
    int  columns = 0;
    int  rows = 0;
};

static SauceInfo ParseSauce(const std::vector<std::uint8_t>& bytes)
{
    // SAUCE record is 128 bytes at EOF, preceded by optional 0x1A.
    // Layout: "SAUCE" + version + title/author/group/date + datatype/filetype + tinfo1/tinfo2...
    // For ANSI, tinfo1 = columns, tinfo2 = rows, little-endian 16-bit.
    SauceInfo out;
    if (bytes.size() < 128)
        return out;

    const size_t sauce_off = bytes.size() - 128;
    if (!(bytes[sauce_off + 0] == 'S' &&
          bytes[sauce_off + 1] == 'A' &&
          bytes[sauce_off + 2] == 'U' &&
          bytes[sauce_off + 3] == 'C' &&
          bytes[sauce_off + 4] == 'E'))
        return out;

    auto u16le = [&](size_t off) -> int
    {
        const std::uint16_t v = (std::uint16_t)bytes[sauce_off + off + 0] |
                                ((std::uint16_t)bytes[sauce_off + off + 1] << 8);
        return (int)v;
    };

    // SAUCE spec offsets (within the 128-byte record):
    // - DataType:  90 (1 byte)
    // - FileType:  91 (1 byte)
    // - TInfo1:    92..93 (u16 LE)  -> columns (for ANSI)
    // - TInfo2:    94..95 (u16 LE)  -> rows    (for ANSI)
    const int cols = u16le(92);
    const int rows = u16le(94);
    if (cols > 0 && cols <= 4096)
    {
        out.valid = true;
        out.columns = cols;
        out.rows = (rows > 0 && rows <= 16384) ? rows : 0;
    }
    return out;
}

static void ParseParams(std::string_view s, std::vector<int>& out)
{
    out.clear();
    int cur = 0;
    bool have = false;
    for (char ch : s)
    {
        if (ch >= '0' && ch <= '9')
        {
            have = true;
            cur = cur * 10 + (ch - '0');
            continue;
        }
        if (ch == ';')
        {
            out.push_back(have ? cur : 0);
            cur = 0;
            have = false;
            continue;
        }
        // Ignore other chars (e.g. '?').
    }
    out.push_back(have ? cur : 0);
}

enum class Mode
{
    Palette16,
    Xterm256,
    TrueColor,
};

struct Pen
{
    bool bold = false;
    bool blink = false;
    bool invert = false;

    // Track palette indices when applicable so we can reproduce libansilove's
    // invert behavior for bright colors (foreground&8).
    Mode fg_mode = Mode::Palette16;
    Mode bg_mode = Mode::Palette16;
    int  fg_idx = 7; // ANSI default foreground
    int  bg_idx = 0; // ANSI default background

    AnsiCanvas::Color32 fg = 0;
    AnsiCanvas::Color32 bg = 0;
};

static inline AnsiCanvas::Color32 ColorFromAnsi16(int idx)
{
    // Reuse xterm256 for indices 0..15 (the canonical palette used across this codebase).
    return (AnsiCanvas::Color32)xterm256::Color32ForIndex(std::clamp(idx, 0, 15));
}

static inline void ApplyDefaults(const Options& opt, Pen& pen)
{
    pen.bold = false;
    pen.blink = false;
    pen.invert = false;

    pen.fg_mode = Mode::Palette16;
    pen.bg_mode = Mode::Palette16;
    pen.fg_idx = 7;
    pen.bg_idx = 0;

    const AnsiCanvas::Color32 def_fg = (opt.default_fg != 0) ? opt.default_fg : ColorFromAnsi16(7);
    const AnsiCanvas::Color32 def_bg = (opt.default_bg != 0) ? opt.default_bg : ColorFromAnsi16(0);
    pen.fg = def_fg;
    pen.bg = def_bg;
}

static inline char32_t DecodeTextCp(const Options& opt, const std::vector<std::uint8_t>& bytes, size_t& i)
{
    (void)opt;
    const std::uint8_t b = bytes[i];
    i += 1;
    // Many ANSI art tools emit NUL bytes for "blank"; treat as space.
    // Also treat other control bytes (0x01..0x1F) as spaces to avoid injecting
    // "control glyphs" into modern Unicode fonts.
    if (b < 0x20u)
        return U' ';
    return kCp437[b];
}

static inline bool DecodeTextUtf8(const Options& opt, const std::vector<std::uint8_t>& bytes, size_t& i, char32_t& out_cp)
{
    (void)opt;
    size_t before = i;
    if (DecodeOneUtf8(bytes.data(), bytes.size(), i, out_cp))
        return true;
    i = before + 1;
    out_cp = U'\uFFFD';
    return false;
}
} // namespace

bool ImportAnsiFileToCanvas(const std::string& path, AnsiCanvas& out_canvas, std::string& err, const Options& options)
{
    err.clear();

    std::string rerr;
    const auto bytes = ReadAllBytes(path, rerr);
    if (!rerr.empty())
    {
        err = rerr;
        return false;
    }

    // Prefer SAUCE columns only when caller requests "auto" columns.
    const SauceInfo sauce = ParseSauce(bytes);
    int columns = ClampColumns(options.columns > 0 ? options.columns : 80);
    if (sauce.valid && options.columns <= 0)
        columns = ClampColumns(sauce.columns);
    if (bytes.empty())
    {
        out_canvas = AnsiCanvas(columns);
        out_canvas.EnsureRowsPublic(1);
        return true;
    }

    // Document state we build and apply as a ProjectState for efficient import.
    int row = 0;
    int col = 0;
    int rowMax = 0;
    int colMax = 0;
    int saved_row = 0;
    int saved_col = 0;

    Pen pen;
    ApplyDefaults(options, pen);

    // Auto-detect UTF-8 ANSI art vs classic CP437 ANSI art.
    // If the content contains ESC, we assume "classic ANSI" unless the caller forces UTF-8.
    bool decode_cp437 = options.cp437;
    if (options.cp437 && !ContainsEsc(bytes) && LooksLikeUtf8Text(bytes))
        decode_cp437 = false;

    // We build a single layer (Base).
    std::vector<char32_t> cells;
    std::vector<AnsiCanvas::Color32> fg;
    std::vector<AnsiCanvas::Color32> bg;

    auto ensure_rows = [&](int rows_needed)
    {
        if (rows_needed < 1) rows_needed = 1;
        const size_t need = (size_t)rows_needed * (size_t)columns;
        if (cells.size() < need)
        {
            cells.resize(need, U' ');
            fg.resize(need, 0);
            bg.resize(need, pen.bg); // default background (black)
        }
    };

    ensure_rows(1);

    auto idx_of = [&](int r, int c) -> size_t
    {
        if (r < 0) r = 0;
        if (c < 0) c = 0;
        if (c >= columns) c = columns - 1;
        return (size_t)r * (size_t)columns + (size_t)c;
    };

    auto put = [&](char32_t cp)
    {
        if (col == columns)
        {
            row += 1;
            col = 0;
        }

        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (col >= columns) col = columns - 1;

        ensure_rows(row + 1);
        const size_t at = idx_of(row, col);

        // Apply invert per libansilove rules when we have 16-color palette indices.
        AnsiCanvas::Color32 out_fg = pen.fg;
        AnsiCanvas::Color32 out_bg = pen.bg;
        if (pen.invert)
        {
            if (pen.fg_mode == Mode::Palette16 && pen.bg_mode == Mode::Palette16)
            {
                const int fg_idx = std::clamp(pen.fg_idx, 0, 15);
                const int bg_idx = std::clamp(pen.bg_idx, 0, 15);
                const int inv_bg = fg_idx % 8;
                const int inv_fg = bg_idx + (fg_idx & 8);
                out_bg = ColorFromAnsi16(inv_bg);
                out_fg = ColorFromAnsi16(inv_fg);
            }
            else
            {
                std::swap(out_fg, out_bg);
            }
        }

        cells[at] = cp;
        fg[at] = out_fg;
        bg[at] = out_bg;

        if (row > rowMax) rowMax = row;
        if (col > colMax) colMax = col;
        col += 1;
    };

    enum class State
    {
        Text,
        Sequence,
        End,
    };

    State state = State::Text;
    size_t i = 0;

    // ANSI_SEQUENCE_MAX_LENGTH in libansilove is 14. We allow a bit more for modern SGR forms.
    static constexpr size_t kSeqMaxLen = 64;

    auto param = [&](const std::vector<int>& p, size_t idx, int def) -> int
    {
        if (idx >= p.size()) return def;
        return p[idx];
    };

    while (i < bytes.size() && state != State::End)
    {
        // libansilove wraps before processing the next character.
        if (state == State::Text && col == columns)
        {
            row += 1;
            col = 0;
        }

        const std::uint8_t b = bytes[i];
        if (state == State::Text)
        {
            switch (b)
            {
                case LF:
                    row += 1;
                    col = 0;
                    if (row > rowMax) rowMax = row;
                    i += 1;
                    break;
                case CR:
                    // Carriage return: return to start of line.
                    col = 0;
                    i += 1;
                    break;
                case TAB:
                {
                    // Emulate 8-column tab stops (and actually fill spaces so the canvas is stable).
                    const int tab_w = 8;
                    const int next = ((col / tab_w) + 1) * tab_w;
                    while (col < std::min(next, columns))
                        put(U' ');
                    i += 1;
                    break;
                }
                case SUB:
                    state = State::End;
                    break;
                case ESC:
                    if (i + 1 < bytes.size() && bytes[i + 1] == (std::uint8_t)'[')
                    {
                        state = State::Sequence;
                        i += 2; // skip ESC[
                    }
                    else
                    {
                        // Unsupported ESC sequence: skip one byte.
                        i += 1;
                    }
                    break;
                default:
                {
                    // Normal text.
                    char32_t cp = U'\0';
                    if (decode_cp437)
                        cp = DecodeTextCp(options, bytes, i);
                    else
                        DecodeTextUtf8(options, bytes, i, cp);

                    // For CP437, bytes 0x01..0x1F are valid glyphs (☺☻♥…).
                    // For UTF-8, treat ASCII control codes as non-printing.
                    if (decode_cp437 || cp >= 0x20)
                        put(cp);
                    break;
                }
            }
            continue;
        }

        // STATE_SEQUENCE: parse CSI parameters until final byte.
        if (state == State::Sequence)
        {
            const size_t seq_start = i;
            size_t j = i;
            size_t consumed = 0;
            char final = '\0';
            while (j < bytes.size() && consumed < kSeqMaxLen)
            {
                const char ch = (char)bytes[j];
                // Standard CSI final byte is 0x40..0x7E.
                // Some tooling (e.g. iCE Draw/icy tools) emits CSI sequences ending in '!' (0x21).
                // We treat '!' as a terminator too so we don't desync.
                if (((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7E) || ch == '!')
                {
                    final = ch;
                    break;
                }
                j++;
                consumed++;
            }

            if (final == '\0')
            {
                // Malformed/truncated: bail out of sequence parsing.
                state = State::Text;
                i = std::min(bytes.size(), seq_start + consumed + 1);
                continue;
            }

            const std::string_view params_view(reinterpret_cast<const char*>(bytes.data() + seq_start), j - seq_start);
            std::vector<int> params;
            ParseParams(params_view, params);

            // Apply sequence effect.
            if (final == 'H' || final == 'f')
            {
                // CUP/HVP: 1-based row/col.
                const int r1 = param(params, 0, 1);
                const int c1 = param(params, 1, 1);
                row = std::max(0, (r1 ? r1 : 1) - 1);
                col = std::max(0, (c1 ? c1 : 1) - 1);
            }
            else if (final == 'A') // CUU
            {
                const int n = param(params, 0, 0);
                row -= (n ? n : 1);
                if (row < 0) row = 0;
            }
            else if (final == 'B') // CUD
            {
                const int n = param(params, 0, 0);
                row += (n ? n : 1);
            }
            else if (final == 'C') // CUF
            {
                const int n = param(params, 0, 0);
                col += (n ? n : 1);
                if (col > columns) col = columns;
            }
            else if (final == 'D') // CUB
            {
                const int n = param(params, 0, 0);
                col -= (n ? n : 1);
                if (col < 0) col = 0;
            }
            else if (final == 'G') // CHA (1-based column)
            {
                const int c1 = param(params, 0, 1);
                col = std::max(0, (c1 ? c1 : 1) - 1);
            }
            else if (final == 's') // save cursor
            {
                saved_row = row;
                saved_col = col;
            }
            else if (final == 'u') // restore cursor
            {
                row = saved_row;
                col = saved_col;
            }
            else if (final == 'J') // erase display
            {
                const int v = param(params, 0, 0);
                if (v == 2)
                {
                    row = 0;
                    col = 0;
                    saved_row = 0;
                    saved_col = 0;
                    rowMax = 0;
                    colMax = 0;
                    cells.assign((size_t)columns, U' ');
                    fg.assign((size_t)columns, 0);
                    bg.assign((size_t)columns, pen.bg);
                }
            }
            else if (final == 'm') // SGR
            {
                if (params.empty())
                    params.push_back(0);

                for (size_t k = 0; k < params.size(); ++k)
                {
                    const int code = params[k];
                    if (code == 0)
                    {
                        ApplyDefaults(options, pen);
                    }
                    else if (code == 1)
                    {
                        // libansilove: foreground += 8 (unless workbench), and bold=true.
                        if (pen.fg_mode == Mode::Palette16 && pen.fg_idx >= 0 && pen.fg_idx < 8)
                        {
                            pen.fg_idx += 8;
                            pen.fg = ColorFromAnsi16(pen.fg_idx);
                        }
                        pen.bold = true;
                    }
                    else if (code == 5)
                    {
                        // ICE colors: blink -> bright background.
                        if (options.icecolors && pen.bg_mode == Mode::Palette16 && pen.bg_idx >= 0 && pen.bg_idx < 8)
                        {
                            pen.bg_idx += 8;
                            pen.bg = ColorFromAnsi16(pen.bg_idx);
                        }
                        pen.blink = true;
                    }
                    else if (code == 7)
                    {
                        pen.invert = true;
                    }
                    else if (code == 27)
                    {
                        pen.invert = false;
                    }
                    else if (code == 22)
                    {
                        pen.bold = false;
                    }
                    else if (code == 39)
                    {
                        // Reset fg to default.
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = 7;
                        pen.fg = (options.default_fg != 0) ? options.default_fg : ColorFromAnsi16(7);
                    }
                    else if (code == 49)
                    {
                        // Reset bg to default.
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = 0;
                        pen.bg = (options.default_bg != 0) ? options.default_bg : ColorFromAnsi16(0);
                    }
                    else if (code >= 30 && code <= 37)
                    {
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = code - 30;
                        if (pen.bold)
                            pen.fg_idx += 8;
                        pen.fg = ColorFromAnsi16(pen.fg_idx);
                    }
                    else if (code >= 90 && code <= 97)
                    {
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = (code - 90) + 8;
                        pen.fg = ColorFromAnsi16(pen.fg_idx);
                    }
                    else if (code >= 40 && code <= 47)
                    {
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = code - 40;
                        if (pen.blink && options.icecolors)
                            pen.bg_idx += 8;
                        pen.bg = ColorFromAnsi16(pen.bg_idx);
                    }
                    else if (code >= 100 && code <= 107)
                    {
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = (code - 100) + 8;
                        pen.bg = ColorFromAnsi16(pen.bg_idx);
                    }
                    else if (code == 38 || code == 48)
                    {
                        const bool is_fg = (code == 38);
                        const int mode = param(params, k + 1, -1);
                        if (mode == 5)
                        {
                            const int idx = param(params, k + 2, -1);
                            if (idx >= 0 && idx <= 255)
                            {
                                const auto col32 = (AnsiCanvas::Color32)xterm256::Color32ForIndex(idx);
                                if (is_fg)
                                {
                                    pen.fg_mode = Mode::Xterm256;
                                    pen.fg_idx = idx;
                                    pen.fg = col32;
                                }
                                else
                                {
                                    pen.bg_mode = Mode::Xterm256;
                                    pen.bg_idx = idx;
                                    pen.bg = col32;
                                }
                            }
                            k += 2;
                        }
                        else if (mode == 2)
                        {
                            const int rr = param(params, k + 2, -1);
                            const int gg = param(params, k + 3, -1);
                            const int bb = param(params, k + 4, -1);
                            if (rr >= 0 && gg >= 0 && bb >= 0)
                            {
                                const auto col32 = PackImGuiCol32((std::uint8_t)std::clamp(rr, 0, 255),
                                                                 (std::uint8_t)std::clamp(gg, 0, 255),
                                                                 (std::uint8_t)std::clamp(bb, 0, 255));
                                if (is_fg)
                                {
                                    pen.fg_mode = Mode::TrueColor;
                                    pen.fg = col32;
                                }
                                else
                                {
                                    pen.bg_mode = Mode::TrueColor;
                                    pen.bg = col32;
                                }
                            }
                            k += 4;
                        }
                    }
                }
            }
            else if (final == 't')
            {
                // PabloDraw 24-bit sequences: ESC[0;R;G;Bt (bg), ESC[1;R;G;Bt (fg)
                if (params.size() >= 4)
                {
                    const int which = params[0];
                    const int rr = params[1];
                    const int gg = params[2];
                    const int bb = params[3];
                    const auto col32 = PackImGuiCol32((std::uint8_t)std::clamp(rr, 0, 255),
                                                     (std::uint8_t)std::clamp(gg, 0, 255),
                                                     (std::uint8_t)std::clamp(bb, 0, 255));
                    if (which == 0)
                    {
                        pen.bg_mode = Mode::TrueColor;
                        pen.bg = col32;
                    }
                    else if (which == 1)
                    {
                        pen.fg_mode = Mode::TrueColor;
                        pen.fg = col32;
                    }
                }
            }
            else if (final == 'p' || final == 'h' || final == 'l' || final == 'K' || final == '!')
            {
                // Intentionally ignored (libansilove ignores these too).
            }

            state = State::Text;
            i = j + 1; // consume final byte
            continue;
        }
    }

    const int out_rows = std::max(1, rowMax + 1);
    const int out_cols = columns; // fixed
    ensure_rows(out_rows);

    AnsiCanvas::ProjectState st;
    st.version = 1;
    st.undo_limit = 256;
    st.current.columns = out_cols;
    st.current.rows = out_rows;
    st.current.active_layer = 0;
    st.current.caret_row = 0;
    st.current.caret_col = 0;
    st.current.layers.clear();
    st.current.layers.resize(1);
    st.current.layers[0].name = "Base";
    st.current.layers[0].visible = true;
    st.current.layers[0].cells = std::move(cells);
    st.current.layers[0].fg = std::move(fg);
    st.current.layers[0].bg = std::move(bg);

    AnsiCanvas canvas(out_cols);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply imported ANSI state." : apply_err;
        return false;
    }

    out_canvas = std::move(canvas);
    return true;
}
} // namespace ansi_importer


