#include "io/formats/xbin.h"

#include "core/color_system.h"
#include "io/formats/sauce.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace formats
{
namespace xbin
{
const std::vector<std::string_view>& ImportExtensions()
{
    static const std::vector<std::string_view> exts = {"xb"};
    return exts;
}

const std::vector<std::string_view>& ExportExtensions()
{
    static const std::vector<std::string_view> exts = {"xb"};
    return exts;
}

namespace
{
static constexpr std::uint8_t kXbinMagic[4] = {'X', 'B', 'I', 'N'};

// CP437 mapping table (0..255) -> Unicode codepoints.
// Keep in sync with formats::ansi CP437 table (duplicated intentionally to keep formats independent).
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

static inline std::uint16_t ReadU16LE(const std::vector<std::uint8_t>& b, size_t off)
{
    return (std::uint16_t)b[off] | ((std::uint16_t)b[off + 1] << 8);
}

static inline void WriteU16LE(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back((std::uint8_t)(v & 0xFFu));
    out.push_back((std::uint8_t)((v >> 8) & 0xFFu));
}

static std::vector<std::uint8_t> ReadAllBytes(const std::string& path, std::string& err)
{
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file.";
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0)
    {
        err = "Failed to read file size.";
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes((size_t)n);
    if (!bytes.empty())
        in.read((char*)bytes.data(), (std::streamsize)bytes.size());
    if (!in && !bytes.empty())
    {
        err = "Failed to read file.";
        return {};
    }
    return bytes;
}

struct Header
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t font_height = 16;
    bool has_palette = false;
    bool has_font = false;
    bool compressed = false;
    bool nonblink = false;
    bool mode_512 = false;
};

static bool ParseHeader(const std::vector<std::uint8_t>& payload, Header& out, size_t& io_off, std::string& err)
{
    err.clear();
    io_off = 0;
    if (payload.size() < 11)
    {
        err = "File too small to be an XBin.";
        return false;
    }
    if (!std::equal(std::begin(kXbinMagic), std::end(kXbinMagic), payload.begin()))
    {
        err = "Not an XBin file (missing XBIN header).";
        return false;
    }
    if (payload[4] != 0x1A)
    {
        err = "Not an XBin file (missing Ctrl-Z byte after XBIN).";
        return false;
    }

    const std::uint16_t w = ReadU16LE(payload, 5);
    const std::uint16_t h = ReadU16LE(payload, 7);
    std::uint8_t font_h = payload[9];
    const std::uint8_t flags = payload[10];

    if (w == 0 || h == 0)
    {
        err = "Invalid XBin dimensions (width/height must be > 0).";
        return false;
    }
    if (font_h == 0)
        font_h = 16; // common convention
    if (font_h < 1 || font_h > 32)
    {
        err = "Invalid XBin font height (must be 1..32).";
        return false;
    }

    out.width = w;
    out.height = h;
    out.font_height = font_h;
    out.has_palette = (flags & 0x01u) != 0;
    out.has_font = (flags & 0x02u) != 0;
    out.compressed = (flags & 0x04u) != 0;
    out.nonblink = (flags & 0x08u) != 0;
    out.mode_512 = (flags & 0x10u) != 0;

    if (out.mode_512 && !out.has_font)
    {
        err = "XBin 512-character mode requires an embedded font.";
        return false;
    }

    io_off = 11;
    return true;
}

static bool ReadPalette(const std::vector<std::uint8_t>& payload,
                        size_t& io_off,
                        std::array<AnsiCanvas::Color32, 16>& out_palette,
                        std::string& err)
{
    err.clear();
    if (io_off + 48 > payload.size())
    {
        err = "Truncated XBin palette.";
        return false;
    }
    for (int i = 0; i < 16; ++i)
    {
        const std::uint8_t r6 = payload[io_off + i * 3 + 0];
        const std::uint8_t g6 = payload[io_off + i * 3 + 1];
        const std::uint8_t b6 = payload[io_off + i * 3 + 2];
        // 6-bit VGA -> 8-bit expansion (common: v<<2 | v>>4).
        const std::uint8_t r8 = (std::uint8_t)((r6 << 2) | (r6 >> 4));
        const std::uint8_t g8 = (std::uint8_t)((g6 << 2) | (g6 >> 4));
        const std::uint8_t b8 = (std::uint8_t)((b6 << 2) | (b6 >> 4));
        out_palette[i] = PackImGuiCol32(r8, g8, b8);
    }
    io_off += 48;
    return true;
}

static bool ReadFont(const std::vector<std::uint8_t>& payload,
                     size_t& io_off,
                     std::uint8_t font_height,
                     bool mode_512,
                     std::vector<std::uint8_t>& out_bitmap,
                     std::string& err)
{
    err.clear();
    const size_t chars = mode_512 ? 512 : 256;
    const size_t bytes = (size_t)font_height * chars;
    if (io_off + bytes > payload.size())
    {
        err = "Truncated XBin font data.";
        return false;
    }
    out_bitmap.assign(payload.begin() + (ptrdiff_t)io_off, payload.begin() + (ptrdiff_t)(io_off + bytes));
    io_off += bytes;
    return true;
}

static bool DecodeCompressedRow(const std::vector<std::uint8_t>& payload,
                               size_t& io_off,
                               int width,
                               std::vector<std::uint8_t>& out_chars,
                               std::vector<std::uint8_t>& out_attrs,
                               std::string& err)
{
    err.clear();
    out_chars.clear();
    out_attrs.clear();
    out_chars.reserve((size_t)width);
    out_attrs.reserve((size_t)width);

    while ((int)out_chars.size() < width)
    {
        if (io_off >= payload.size())
        {
            err = "Truncated XBin compressed image data.";
            return false;
        }
        const std::uint8_t tag = payload[io_off++];
        const std::uint8_t type = (tag >> 6) & 0x03u;
        const int count = (int)(tag & 0x3Fu) + 1; // 1..64

        if ((int)out_chars.size() + count > width)
        {
            err = "Invalid XBin compressed row (run exceeds row width).";
            return false;
        }

        if (type == 0)
        {
            // No compression: count * (char,attr)
            const size_t need = (size_t)count * 2;
            if (io_off + need > payload.size())
            {
                err = "Truncated XBin compressed image data.";
                return false;
            }
            for (int i = 0; i < count; ++i)
            {
                out_chars.push_back(payload[io_off++]);
                out_attrs.push_back(payload[io_off++]);
            }
        }
        else if (type == 1)
        {
            // Character compression: char, then count attrs
            if (io_off + 1 + (size_t)count > payload.size())
            {
                err = "Truncated XBin compressed image data.";
                return false;
            }
            const std::uint8_t ch = payload[io_off++];
            for (int i = 0; i < count; ++i)
            {
                out_chars.push_back(ch);
                out_attrs.push_back(payload[io_off++]);
            }
        }
        else if (type == 2)
        {
            // Attribute compression: attr, then count chars
            if (io_off + 1 + (size_t)count > payload.size())
            {
                err = "Truncated XBin compressed image data.";
                return false;
            }
            const std::uint8_t at = payload[io_off++];
            for (int i = 0; i < count; ++i)
            {
                out_chars.push_back(payload[io_off++]);
                out_attrs.push_back(at);
            }
        }
        else
        {
            // Character/Attribute compression: (char,attr) pair
            if (io_off + 2 > payload.size())
            {
                err = "Truncated XBin compressed image data.";
                return false;
            }
            const std::uint8_t ch = payload[io_off++];
            const std::uint8_t at = payload[io_off++];
            for (int i = 0; i < count; ++i)
            {
                out_chars.push_back(ch);
                out_attrs.push_back(at);
            }
        }
    }

    return true;
}

static std::uint8_t UnicodeToCp437Byte(char32_t cp)
{
    static const std::unordered_map<char32_t, std::uint8_t> map = []() {
        std::unordered_map<char32_t, std::uint8_t> m;
        m.reserve(256);
        for (size_t i = 0; i < 256; ++i)
            m.emplace(kCp437[i], (std::uint8_t)i);
        return m;
    }();

    auto it = map.find(cp);
    return (it == map.end()) ? (std::uint8_t)'?' : it->second;
}

static std::uint8_t ToVga6(std::uint8_t v8)
{
    // Map 0..255 -> 0..63 with rounding.
    return (std::uint8_t)((v8 * 63u + 127u) / 255u);
}

static void WriteDefaultPalette(std::vector<std::uint8_t>& out)
{
    // Use xterm low-16 RGB as the palette entries for deterministic roundtrips.
    auto& cs = phos::color::GetColorSystem();
    const phos::color::PaletteInstanceId pal16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16);
    const phos::color::Palette* p = cs.Palettes().Get(pal16);
    if (!p || p->rgb.size() < 16)
        return;
    for (int i = 0; i < 16; ++i)
    {
        const phos::color::Rgb8 rgb = p->rgb[(size_t)i];
        out.push_back(ToVga6(rgb.r));
        out.push_back(ToVga6(rgb.g));
        out.push_back(ToVga6(rgb.b));
    }
}

static void BuildDefaultPalette32(std::array<AnsiCanvas::Color32, 16>& out_pal32)
{
    auto& cs = phos::color::GetColorSystem();
    const phos::color::PaletteInstanceId pal16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16);
    const phos::color::Palette* p = cs.Palettes().Get(pal16);
    if (!p || p->rgb.size() < 16)
        return;
    for (int i = 0; i < 16; ++i)
        out_pal32[i] = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal16, phos::color::ColorIndex{(std::uint16_t)i});
}

static void EncodeRowRle(const std::uint8_t* ch, const std::uint8_t* at, int width, std::vector<std::uint8_t>& out)
{
    auto run_both = [&](int x) -> int {
        const std::uint8_t c0 = ch[x];
        const std::uint8_t a0 = at[x];
        int n = 1;
        while (x + n < width && n < 64 && ch[x + n] == c0 && at[x + n] == a0)
            n++;
        return n;
    };
    auto run_char = [&](int x) -> int {
        const std::uint8_t c0 = ch[x];
        int n = 1;
        while (x + n < width && n < 64 && ch[x + n] == c0)
            n++;
        return n;
    };
    auto run_attr = [&](int x) -> int {
        const std::uint8_t a0 = at[x];
        int n = 1;
        while (x + n < width && n < 64 && at[x + n] == a0)
            n++;
        return n;
    };
    auto profitable_at = [&](int x) -> bool {
        const int rb = run_both(x);
        if (rb >= 2) return true;
        const int rc = run_char(x);
        if (rc >= 3) return true;
        const int ra = run_attr(x);
        if (ra >= 3) return true;
        return false;
    };

    int x = 0;
    while (x < width)
    {
        const int rb = run_both(x);
        const int rc = run_char(x);
        const int ra = run_attr(x);

        // Choose best compressed form if it yields real savings.
        enum Kind : int { None = 0, Char = 1, Attr = 2, Both = 3 };
        Kind kind = None;
        int len = 1;

        if (rb >= 2)
        {
            kind = Both;
            len = rb;
        }
        if (rc >= 3)
        {
            const int save = (2 * rc) - (2 + rc); // raw - (tag + char + attrs)
            const int cur_save = (kind == Both) ? ((2 * len) - 3) : -9999;
            if (save > cur_save)
            {
                kind = Char;
                len = rc;
            }
        }
        if (ra >= 3)
        {
            const int save = (2 * ra) - (2 + ra); // raw - (tag + attr + chars)
            int cur_save = -9999;
            if (kind == Both) cur_save = (2 * len) - 3;
            else if (kind == Char) cur_save = (2 * len) - (2 + len);
            if (save > cur_save)
            {
                kind = Attr;
                len = ra;
            }
        }

        if (kind == None)
        {
            // Emit a "no compression" chunk, but try not to swallow upcoming profitable runs.
            int n = 1;
            while (x + n < width && n < 64 && !profitable_at(x + n))
                n++;
            len = n;
        }

        const std::uint8_t tag = (std::uint8_t)(((int)kind << 6) | ((len - 1) & 0x3F));
        out.push_back(tag);
        if (kind == None)
        {
            for (int i = 0; i < len; ++i)
            {
                out.push_back(ch[x + i]);
                out.push_back(at[x + i]);
            }
        }
        else if (kind == Char)
        {
            out.push_back(ch[x]);
            for (int i = 0; i < len; ++i)
                out.push_back(at[x + i]);
        }
        else if (kind == Attr)
        {
            out.push_back(at[x]);
            for (int i = 0; i < len; ++i)
                out.push_back(ch[x + i]);
        }
        else
        {
            out.push_back(ch[x]);
            out.push_back(at[x]);
        }

        x += len;
    }
}
} // namespace

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options)
{
    err.clear();

    sauce::Parsed sp;
    std::string serr;
    (void)sauce::ParseFromBytes(bytes, sp, serr, true /* SAUCE fields are spec'd as CP437 */);
    const size_t payload_len = sp.record.present ? std::min(sp.payload_size, bytes.size()) : bytes.size();
    const std::vector<std::uint8_t> payload(bytes.begin(), bytes.begin() + payload_len);

    Header hdr;
    size_t off = 0;
    if (!ParseHeader(payload, hdr, off, err))
        return false;

    std::array<AnsiCanvas::Color32, 16> pal32{};
    BuildDefaultPalette32(pal32);
    if (hdr.has_palette)
    {
        if (!ReadPalette(payload, off, pal32, err))
            return false;
    }
    std::vector<std::uint8_t> embedded_font_bitmap;
    if (hdr.has_font)
    {
        if (!ReadFont(payload, off, hdr.font_height, hdr.mode_512, embedded_font_bitmap, err))
            return false;
    }

    const int cols = (int)hdr.width;
    const int rows = (int)hdr.height;

    std::vector<char32_t> cells((size_t)cols * (size_t)rows, U' ');
    std::vector<AnsiCanvas::Color32> fg((size_t)cols * (size_t)rows, 0);
    std::vector<AnsiCanvas::Color32> bg((size_t)cols * (size_t)rows, 0);

    const int embedded_glyph_count = hdr.mode_512 ? 512 : 256;
    const bool use_embedded_font = hdr.has_font && !embedded_font_bitmap.empty();

    auto decode_cp = [&](std::uint8_t b) -> char32_t {
        if (options.decode_cp437)
        {
            // Treat NUL as space (common "blank" convention).
            if (b == 0)
                return U' ';
            return kCp437[b];
        }
        if (b < 0x80)
            return (char32_t)b;
        return U'\uFFFD';
    };

    const bool warn_512 = hdr.mode_512;
    (void)warn_512;

    std::vector<std::uint8_t> row_ch;
    std::vector<std::uint8_t> row_at;

    auto apply_row = [&](int y, const std::vector<std::uint8_t>& ch, const std::vector<std::uint8_t>& at) {
        for (int x = 0; x < cols; ++x)
        {
            const std::uint8_t c = ch[(size_t)x];
            const std::uint8_t a = at[(size_t)x];

            int fg_idx = 0;
            int bg_idx = 0;
            if (hdr.mode_512)
            {
                // In 512-char mode, bit3 selects font page; foreground is only 0..7.
                fg_idx = (int)(a & 0x07u);
            }
            else
            {
                fg_idx = (int)(a & 0x0Fu);
            }

            if (hdr.nonblink)
            {
                bg_idx = (int)((a >> 4) & 0x0Fu);
            }
            else
            {
                // Blink mode: background only 0..7, bit7 is blink.
                bg_idx = (int)((a >> 4) & 0x07u);
            }

            fg_idx = std::clamp(fg_idx, 0, 15);
            bg_idx = std::clamp(bg_idx, 0, 15);

            const size_t idx = (size_t)y * (size_t)cols + (size_t)x;
            if (use_embedded_font)
            {
                std::uint16_t gi = (std::uint16_t)c;
                if (hdr.mode_512 && (a & 0x08u))
                    gi = (std::uint16_t)((std::uint16_t)c + 256u);
                if (gi >= (std::uint16_t)embedded_glyph_count)
                    gi = 0;
                cells[idx] = AnsiCanvas::kEmbeddedGlyphBase + (char32_t)gi;
            }
            else
            {
                cells[idx] = decode_cp(c);
            }
            fg[idx] = pal32[(size_t)fg_idx];
            bg[idx] = pal32[(size_t)bg_idx];
        }
    };

    if (!hdr.compressed)
    {
        const size_t need = (size_t)cols * (size_t)rows * 2;
        if (off + need > payload.size())
        {
            err = "Truncated XBin image data.";
            return false;
        }

        for (int y = 0; y < rows; ++y)
        {
            row_ch.resize((size_t)cols);
            row_at.resize((size_t)cols);
            for (int x = 0; x < cols; ++x)
            {
                row_ch[(size_t)x] = payload[off++];
                row_at[(size_t)x] = payload[off++];
            }
            apply_row(y, row_ch, row_at);
        }
    }
    else
    {
        for (int y = 0; y < rows; ++y)
        {
            if (!DecodeCompressedRow(payload, off, cols, row_ch, row_at, err))
                return false;
            apply_row(y, row_ch, row_at);
        }
    }

    AnsiCanvas::ProjectState st;
    st.version = 2;
    st.undo_limit = 0; // unlimited by default
    st.current.columns = cols;
    st.current.rows = rows;
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
    // Track palette identity on the canvas (XBin palettes are always 16-color).
    {
        if (hdr.has_palette)
        {
            auto& cs = phos::color::GetColorSystem();
            std::array<phos::color::Rgb8, 16> rgb{};
            for (int i = 0; i < 16; ++i)
            {
                std::uint8_t r = 0, g = 0, b = 0;
                (void)phos::color::ColorOps::UnpackImGuiAbgr((std::uint32_t)pal32[(size_t)i], r, g, b);
                rgb[(size_t)i] = phos::color::Rgb8{r, g, b};
            }
            (void)cs.Palettes().RegisterDynamic("XBin Palette", rgb);
            st.palette_ref.is_builtin = false;
            st.palette_ref.uid = phos::color::ComputePaletteUid(rgb);
        }
        else
        {
            st.palette_ref.is_builtin = true;
            st.palette_ref.builtin = phos::color::BuiltinPalette::Xterm16;
        }
    }

    // Preserve SAUCE metadata (if present), else populate a minimal XBin-ish record.
    if (sp.record.present)
    {
        st.sauce.present = true;
        st.sauce.title = sp.record.title;
        st.sauce.author = sp.record.author;
        st.sauce.group = sp.record.group;
        st.sauce.date = sp.record.date;
        st.sauce.file_size = sp.record.file_size;
        st.sauce.data_type = sp.record.data_type;
        st.sauce.file_type = sp.record.file_type;
        st.sauce.tinfo1 = sp.record.tinfo1;
        st.sauce.tinfo2 = sp.record.tinfo2;
        st.sauce.tinfo3 = sp.record.tinfo3;
        st.sauce.tinfo4 = sp.record.tinfo4;
        st.sauce.tflags = sp.record.tflags;
        st.sauce.tinfos = sp.record.tinfos;
        st.sauce.comments = sp.record.comments;
    }
    else
    {
        st.sauce.present = true;
        st.sauce.data_type = (std::uint8_t)sauce::DataType::XBin;
        st.sauce.file_type = 0;
        st.sauce.tinfo1 = (std::uint16_t)std::clamp(cols, 0, 65535);
        st.sauce.tinfo2 = (std::uint16_t)std::clamp(rows, 0, 65535);
    }

    AnsiCanvas canvas(cols);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply imported XBin state." : apply_err;
        return false;
    }

    if (use_embedded_font)
    {
        AnsiCanvas::EmbeddedBitmapFont ef;
        ef.cell_w = 8;
        ef.cell_h = (int)hdr.font_height;
        ef.glyph_count = embedded_glyph_count;
        ef.vga_9col_dup = false;
        ef.bitmap = std::move(embedded_font_bitmap);
        canvas.SetEmbeddedFont(std::move(ef));
    }
    out_canvas = std::move(canvas);
    return true;
}

bool ImportFileToCanvas(const std::string& path, AnsiCanvas& out_canvas, std::string& err, const ImportOptions& options)
{
    err.clear();
    std::string rerr;
    const auto bytes = ReadAllBytes(path, rerr);
    if (!rerr.empty())
    {
        err = rerr;
        return false;
    }
    return ImportBytesToCanvas(bytes, out_canvas, err, options);
}

bool ExportCanvasToBytes(const AnsiCanvas& canvas,
                         std::vector<std::uint8_t>& out_bytes,
                         std::string& err,
                         const ExportOptions& options)
{
    err.clear();
    out_bytes.clear();

    if (options.mode_512)
    {
        err = "XBin export: 512-character mode is not supported yet.";
        return false;
    }

    const int cols = std::max(1, canvas.GetColumns());
    const int rows = std::max(1, canvas.GetRows());
    if (cols > 65535 || rows > 65535)
    {
        err = "XBin export: canvas dimensions exceed XBin limits.";
        return false;
    }

    std::array<AnsiCanvas::Color32, 16> pal32{};
    BuildDefaultPalette32(pal32);
    auto& cs = phos::color::GetColorSystem();
    const phos::color::PaletteInstanceId pal16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16);
    phos::color::QuantizePolicy qpol;

    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);

    if (options.include_font)
    {
        if (!embedded_font)
        {
            err = "XBin export: include_font requested but canvas has no embedded font.";
            return false;
        }
        if (ef->glyph_count != 256)
        {
            err = "XBin export: only 256-glyph embedded fonts are supported for export.";
            return false;
        }
        if (ef->cell_w != 8 || ef->cell_h < 1 || ef->cell_h > 32)
        {
            err = "XBin export: embedded font must be 8x(1..32).";
            return false;
        }
    }

    // Header
    out_bytes.insert(out_bytes.end(), std::begin(kXbinMagic), std::end(kXbinMagic));
    out_bytes.push_back(0x1A);
    WriteU16LE(out_bytes, (std::uint16_t)cols);
    WriteU16LE(out_bytes, (std::uint16_t)rows);
    out_bytes.push_back(options.include_font ? (std::uint8_t)ef->cell_h : (std::uint8_t)16);

    std::uint8_t flags = 0;
    if (options.include_palette) flags |= 0x01u;
    if (options.include_font) flags |= 0x02u;
    if (options.compress) flags |= 0x04u;
    if (options.nonblink) flags |= 0x08u;
    if (options.mode_512) flags |= 0x10u;
    out_bytes.push_back(flags);

    if (options.include_palette)
        WriteDefaultPalette(out_bytes);

    if (options.include_font)
        out_bytes.insert(out_bytes.end(), ef->bitmap.begin(), ef->bitmap.end());

    // Gather cell data and quantize to 16-color indices.
    std::vector<std::uint8_t> ch((size_t)cols * (size_t)rows);
    std::vector<std::uint8_t> at((size_t)cols * (size_t)rows);

    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            char32_t cp = U' ';
            AnsiCanvas::Color32 fg32 = 0;
            AnsiCanvas::Color32 bg32 = 0;

            if (options.source == ExportOptions::Source::Composite)
            {
                (void)canvas.GetCompositeCellPublic(y, x, cp, fg32, bg32);
            }
            else
            {
                const int li = canvas.GetActiveLayerIndex();
                cp = canvas.GetLayerCell(li, y, x);
                (void)canvas.GetLayerCellColors(li, y, x, fg32, bg32);
            }

            if (fg32 == 0)
                fg32 = pal32[7];
            if (bg32 == 0)
                bg32 = pal32[0];

            const phos::color::ColorIndex fg_idx =
                phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal16, (std::uint32_t)fg32, qpol);
            const phos::color::ColorIndex bg_idx =
                phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal16, (std::uint32_t)bg32, qpol);
            const int fg_i = fg_idx.IsUnset() ? 7 : (int)std::clamp<int>((int)fg_idx.v, 0, 15);
            const int bg_i = bg_idx.IsUnset() ? 0 : (int)std::clamp<int>((int)bg_idx.v, 0, 15);

            const size_t idx = (size_t)y * (size_t)cols + (size_t)x;
            if (embedded_font && cp >= AnsiCanvas::kEmbeddedGlyphBase && cp < (AnsiCanvas::kEmbeddedGlyphBase + 256))
                ch[idx] = (std::uint8_t)(cp - AnsiCanvas::kEmbeddedGlyphBase);
            else
                ch[idx] = UnicodeToCp437Byte(cp);

            // Encode attribute.
            std::uint8_t attr = 0;
            if (options.nonblink)
            {
                attr = (std::uint8_t)(((bg_i & 0x0F) << 4) | (fg_i & 0x0F));
            }
            else
            {
                // Blink mode: bg 0..7 (we clamp), no blink bit emitted.
                attr = (std::uint8_t)(((bg_i & 0x07) << 4) | (fg_i & 0x0F));
            }
            at[idx] = attr;
        }
    }

    if (!options.compress)
    {
        // Raw image data: [char,attr] pairs in row-major order.
        out_bytes.reserve(out_bytes.size() + (size_t)cols * (size_t)rows * 2);
        for (size_t i = 0; i < ch.size(); ++i)
        {
            out_bytes.push_back(ch[i]);
            out_bytes.push_back(at[i]);
        }
    }
    else
    {
        // Compressed (row by row).
        std::vector<std::uint8_t> row_out;
        for (int y = 0; y < rows; ++y)
        {
            row_out.clear();
            const std::uint8_t* row_ch = ch.data() + (size_t)y * (size_t)cols;
            const std::uint8_t* row_at = at.data() + (size_t)y * (size_t)cols;
            EncodeRowRle(row_ch, row_at, cols, row_out);
            out_bytes.insert(out_bytes.end(), row_out.begin(), row_out.end());
        }
    }

    // Optional SAUCE append.
    if (options.write_sauce)
    {
        sauce::Record r;
        const auto& meta = canvas.GetSauceMeta();
        r.present = true;
        r.title = meta.title;
        r.author = meta.author;
        r.group = meta.group;
        r.date = meta.date;
        r.file_size = (std::uint32_t)out_bytes.size();
        r.data_type = (std::uint8_t)sauce::DataType::XBin;
        r.file_type = 0;
        r.tinfo1 = (std::uint16_t)std::clamp(cols, 0, 65535);
        r.tinfo2 = (std::uint16_t)std::clamp(rows, 0, 65535);
        r.tinfo3 = meta.tinfo3;
        r.tinfo4 = meta.tinfo4;
        r.tflags = meta.tflags;
        r.tinfos = meta.tinfos;
        r.comments = meta.comments;

        std::string aerr;
        const auto with_sauce = sauce::AppendToBytes(out_bytes, r, options.sauce_write_options, aerr);
        if (with_sauce.empty() && !aerr.empty())
        {
            err = aerr;
            return false;
        }
        out_bytes = with_sauce;
    }

    return true;
}

bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options)
{
    err.clear();
    std::vector<std::uint8_t> bytes;
    if (!ExportCanvasToBytes(canvas, bytes, err, options))
        return false;

    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        err = "Failed to open file for writing.";
        return false;
    }
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!out)
    {
        err = "Failed to write file.";
        return false;
    }
    return true;
}
} // namespace xbin
} // namespace formats


