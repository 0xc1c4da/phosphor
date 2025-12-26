#include "io/formats/xbin.h"

#include "core/color_system.h"
#include "core/encodings.h"
#include "core/glyph_id.h"
#include "core/glyph_resolve.h"
#include "core/paths.h"
#include "io/formats/sauce.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
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

// XBIN attributes use IBM PC textmode attribute order (not ANSI/SGR order).
// Our built-in VGA16 palette is in ANSI/SGR order (see core/palette/palette.cpp).
// Remap indices at import so XBIN colors render correctly under the ANSI-ordered palette.
static constexpr std::uint8_t kIbmToAnsi16[16] = {
    0,  // 0 black -> 0 black
    4,  // 1 blue -> 4 blue
    2,  // 2 green -> 2 green
    6,  // 3 cyan -> 6 cyan
    1,  // 4 red -> 1 red
    5,  // 5 magenta -> 5 magenta
    3,  // 6 brown/yellow -> 3 yellow/brown
    7,  // 7 light gray -> 7 light gray
    8,  // 8 dark gray -> 8 dark gray
    12, // 9 light blue -> 12 bright blue
    10, // 10 light green -> 10 bright green
    14, // 11 light cyan -> 14 bright cyan
    9,  // 12 light red -> 9 bright red
    13, // 13 light magenta -> 13 bright magenta
    11, // 14 yellow -> 11 bright yellow
    15, // 15 white -> 15 bright white
};

static constexpr std::uint8_t kAnsiToIbm16[16] = {
    0,  // 0 black
    4,  // 1 red
    2,  // 2 green
    6,  // 3 yellow/brown
    1,  // 4 blue
    5,  // 5 magenta
    3,  // 6 cyan
    7,  // 7 light gray
    8,  // 8 dark gray
    12, // 9 bright red
    10, // 10 bright green
    14, // 11 bright yellow
    9,  // 12 bright blue
    13, // 13 bright magenta
    11, // 14 bright cyan
    15, // 15 bright white
};

static inline AnsiCanvas::Color32 PackImGuiCol32(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Dear ImGui IM_COL32 is ABGR.
    return 0xFF000000u | ((std::uint32_t)b << 16) | ((std::uint32_t)g << 8) | (std::uint32_t)r;
}

static inline void UnpackImGuiCol32(AnsiCanvas::Color32 c, std::uint8_t& out_r, std::uint8_t& out_g, std::uint8_t& out_b)
{
    // Dear ImGui IM_COL32 is ABGR.
    out_r = (std::uint8_t)(c & 0xFFu);
    out_g = (std::uint8_t)((c >> 8) & 0xFFu);
    out_b = (std::uint8_t)((c >> 16) & 0xFFu);
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

static void ReorderPaletteIbmToAnsi(std::array<AnsiCanvas::Color32, 16>& io_pal32)
{
    // io_pal32 currently contains entries in IBM order (as stored in XBIN palette chunk).
    // Convert to ANSI/SGR order to match our palette index semantics.
    std::array<AnsiCanvas::Color32, 16> tmp = io_pal32;
    for (int ibm = 0; ibm < 16; ++ibm)
    {
        const int ansi = (int)kIbmToAnsi16[(size_t)ibm];
        io_pal32[(size_t)ansi] = tmp[(size_t)ibm];
    }
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
    std::uint8_t b = (std::uint8_t)'?';
    (void)phos::encodings::UnicodeToByte(phos::encodings::EncodingId::Cp437, cp, b);
    return b;
}

static std::uint8_t ToVga6(std::uint8_t v8)
{
    // Map 0..255 -> 0..63 with rounding.
    return (std::uint8_t)((v8 * 63u + 127u) / 255u);
}

static void WritePaletteChunk(std::vector<std::uint8_t>& out, phos::color::PaletteInstanceId pal16)
{
    auto& cs = phos::color::GetColorSystem();
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
    // XBin readers assume a classic VGA16 palette when no palette chunk is present.
    const phos::color::PaletteInstanceId pal16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);
    const phos::color::Palette* p = cs.Palettes().Get(pal16);
    if (!p || p->rgb.size() < 16)
        return;
    for (int i = 0; i < 16; ++i)
        out_pal32[i] = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal16, phos::color::ColorIndex{(std::uint16_t)i});
}

struct PaletteDef32
{
    std::string title;
    std::vector<AnsiCanvas::Color32> colors;
};

static bool HexToColor32(const std::string& hex, AnsiCanvas::Color32& out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto to_u8 = [](const std::string& sub) -> std::uint8_t
    {
        return (std::uint8_t)std::strtoul(sub.c_str(), nullptr, 16);
    };

    const std::uint8_t r = to_u8(s.substr(0, 2));
    const std::uint8_t g = to_u8(s.substr(2, 2));
    const std::uint8_t b = to_u8(s.substr(4, 2));
    std::uint8_t a = 255;
    if (s.size() == 8)
        a = to_u8(s.substr(6, 2));

    // Packed colors follow ImGui's IM_COL32 (ABGR).
    out = ((AnsiCanvas::Color32)a << 24) | ((AnsiCanvas::Color32)b << 16) | ((AnsiCanvas::Color32)g << 8) | (AnsiCanvas::Color32)r;
    return true;
}

static bool LoadPalettesFromJson32(const std::string& path,
                                  std::vector<PaletteDef32>& out,
                                  std::string& err)
{
    using nlohmann::json;
    err.clear();
    out.clear();

    std::ifstream f(path);
    if (!f)
    {
        err = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }

    if (!j.is_array())
    {
        err = "Expected top-level JSON array in color-palettes.json";
        return false;
    }

    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        PaletteDef32 def;
        if (auto it = item.find("title"); it != item.end() && it->is_string())
            def.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("colors"); it != item.end() && it->is_array())
        {
            for (const auto& c : *it)
            {
                if (!c.is_string())
                    continue;
                AnsiCanvas::Color32 col = 0;
                if (HexToColor32(c.get<std::string>(), col))
                    def.colors.push_back(col);
            }
        }

        if (!def.colors.empty())
            out.push_back(std::move(def));
    }

    if (out.empty())
    {
        err = "No valid palettes found in color-palettes.json";
        return false;
    }
    return true;
}

static std::string InferPaletteTitleFromPalette16(const std::array<AnsiCanvas::Color32, 16>& pal32,
                                                 const std::vector<PaletteDef32>& palettes)
{
    if (palettes.empty())
        return {};

    auto dist2_rgb = [](AnsiCanvas::Color32 a, AnsiCanvas::Color32 b) -> std::uint32_t
    {
        std::uint8_t ar, ag, ab;
        std::uint8_t br, bg, bb;
        UnpackImGuiCol32(a, ar, ag, ab);
        UnpackImGuiCol32(b, br, bg, bb);
        const int dr = (int)ar - (int)br;
        const int dg = (int)ag - (int)bg;
        const int db = (int)ab - (int)bb;
        return (std::uint32_t)(dr * dr + dg * dg + db * db);
    };

    std::uint64_t best = ~0ull;
    std::string best_title;
    for (const auto& p : palettes)
    {
        if (p.colors.size() < 16)
            continue;
        std::uint64_t s = 0;
        for (int i = 0; i < 16; ++i)
            s += dist2_rgb(pal32[(size_t)i], p.colors[(size_t)i]);
        if (s < best)
        {
            best = s;
            best_title = p.title;
            if (best == 0)
                break;
        }
    }
    return best_title;
}

static bool PaletteEquals16(const std::vector<phos::color::Rgb8>& a, const phos::color::Palette* b)
{
    if (!b || b->rgb.size() != 16 || a.size() != 16)
        return false;
    for (int i = 0; i < 16; ++i)
    {
        if (a[(size_t)i].r != b->rgb[(size_t)i].r ||
            a[(size_t)i].g != b->rgb[(size_t)i].g ||
            a[(size_t)i].b != b->rgb[(size_t)i].b)
            return false;
    }
    return true;
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
        // Palette chunk is stored in IBM order; normalize to ANSI/SGR order for our pipeline.
        ReorderPaletteIbmToAnsi(pal32);
    }
    std::vector<std::uint8_t> embedded_font_bitmap;
    if (hdr.has_font)
    {
        if (!ReadFont(payload, off, hdr.font_height, hdr.mode_512, embedded_font_bitmap, err))
            return false;
    }

    const int cols = (int)hdr.width;
    const int rows = (int)hdr.height;

    std::vector<AnsiCanvas::GlyphId> glyphs((size_t)cols * (size_t)rows, phos::glyph::MakeUnicodeScalar(U' '));
    std::vector<AnsiCanvas::ColorIndex16> fg((size_t)cols * (size_t)rows, AnsiCanvas::kUnsetIndex16);
    std::vector<AnsiCanvas::ColorIndex16> bg((size_t)cols * (size_t)rows, AnsiCanvas::kUnsetIndex16);

    const int embedded_glyph_count = hdr.mode_512 ? 512 : 256;
    const bool use_embedded_font = hdr.has_font && !embedded_font_bitmap.empty();

    auto decode_unicode_scalar = [&](std::uint8_t b) -> char32_t {
        if (options.decode_cp437)
        {
            // Treat NUL as space (common "blank" convention).
            if (b == 0)
                return U' ';
            return phos::encodings::ByteToUnicode(phos::encodings::EncodingId::Cp437, b);
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
            int raw_fg = 0;
            int raw_bg = 0;
            if (hdr.mode_512)
            {
                // In 512-char mode, bit3 selects font page; foreground is only 0..7.
                raw_fg = (int)(a & 0x07u);
            }
            else
            {
                raw_fg = (int)(a & 0x0Fu);
            }

            if (hdr.nonblink)
            {
                raw_bg = (int)((a >> 4) & 0x0Fu);
            }
            else
            {
                // Blink mode: background only 0..7, bit7 is blink.
                raw_bg = (int)((a >> 4) & 0x07u);
            }

            raw_fg = std::clamp(raw_fg, 0, 15);
            raw_bg = std::clamp(raw_bg, 0, 15);

            // Remap IBM attribute indices -> ANSI/SGR palette indices.
            fg_idx = (int)kIbmToAnsi16[(size_t)raw_fg];
            bg_idx = (int)kIbmToAnsi16[(size_t)raw_bg];

            const size_t idx = (size_t)y * (size_t)cols + (size_t)x;
            if (use_embedded_font)
            {
                // XBin convention: NUL bytes are commonly used as "blank". Our GlyphId blank semantics
                // for indexed glyphs treat index 32 as the transparent/space glyph (see core/glyph_id.h),
                // so normalize 0 -> 32 even when an embedded font is present.
                const std::uint16_t base = (c == 0) ? (std::uint16_t)32u : (std::uint16_t)c;
                std::uint16_t gi = base;
                if (hdr.mode_512 && (a & 0x08u))
                    gi = (std::uint16_t)(base + 256u);
                if (gi >= (std::uint16_t)embedded_glyph_count)
                    gi = 0;
                glyphs[idx] = phos::glyph::MakeEmbeddedIndex(gi);
            }
            else
            {
                if (options.decode_cp437)
                {
                    // In non-embedded mode, preserve glyph identity by storing the byte as a BitmapIndex token.
                    // This matches XBin's native representation and avoids lossy Unicode->index remapping later.
                    const std::uint16_t bi = (c == 0) ? (std::uint16_t)32u : (std::uint16_t)c;
                    glyphs[idx] = phos::glyph::MakeBitmapIndex(bi);
                }
                else
                {
                    // Non-CP437 decode: preserve old behavior (Unicode-only; bytes >=0x80 become U+FFFD).
                    glyphs[idx] = phos::glyph::MakeUnicodeScalar(decode_unicode_scalar(c));
                }
            }
            fg[idx] = (AnsiCanvas::ColorIndex16)fg_idx;
            bg[idx] = (AnsiCanvas::ColorIndex16)bg_idx;
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
    // Keep this state at the current in-memory schema version so GlyphId tokens remain meaningful.
    st.version = 13;
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
    st.current.layers[0].cells = std::move(glyphs);
    st.current.layers[0].fg = std::move(fg);
    st.current.layers[0].bg = std::move(bg);

    // Persist embedded font payload (if present) as part of ProjectState so downstream
    // snapshot/undo/restore flows keep it attached.
    if (use_embedded_font)
    {
        AnsiCanvas::EmbeddedBitmapFont ef;
        ef.cell_w = 8;
        ef.cell_h = (int)hdr.font_height;
        ef.glyph_count = embedded_glyph_count;
        ef.vga_9col_dup = false;
        ef.bitmap = embedded_font_bitmap; // copy into ProjectState (we still move it onto canvas below)
        st.embedded_font = std::move(ef);
    }
    // Track palette identity on the canvas (XBin palettes are always 16-color).
    {
        auto& cs = phos::color::GetColorSystem();

        // Build RGB24 list from the decoded palette chunk (or default VGA16 if absent).
        std::vector<phos::color::Rgb8> rgb;
        rgb.reserve(16);
        for (int i = 0; i < 16; ++i)
        {
            std::uint8_t r = 0, g = 0, b = 0;
            UnpackImGuiCol32(pal32[(size_t)i], r, g, b);
            rgb.push_back(phos::color::Rgb8{r, g, b});
        }

        // Prefer builtins when the palette matches exactly (better UX + smaller identity).
        const phos::color::Palette* vga = cs.Palettes().Get(cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16));
        const phos::color::Palette* x16 = cs.Palettes().Get(cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16));
        if (PaletteEquals16(rgb, vga))
        {
            st.palette_ref.is_builtin = true;
            st.palette_ref.builtin = phos::color::BuiltinPalette::Vga16;
            // UI palette selection: follow the core palette identity.
            st.ui_palette_ref = st.palette_ref;
        }
        else if (PaletteEquals16(rgb, x16))
        {
            st.palette_ref.is_builtin = true;
            st.palette_ref.builtin = phos::color::BuiltinPalette::Xterm16;
            // UI palette selection: follow the core palette identity.
            st.ui_palette_ref = st.palette_ref;
        }
        else if (hdr.has_palette)
        {
            // No exact builtin match: register as a dynamic palette.
            std::vector<PaletteDef32> pals;
            std::string perr;
            const std::string pal_path = PhosphorAssetPath("color-palettes.json");
            const bool loaded = LoadPalettesFromJson32(pal_path, pals, perr);
            const std::string inferred = loaded ? InferPaletteTitleFromPalette16(pal32, pals) : std::string{};
            const std::string title = inferred.empty() ? "XBin Palette" : inferred;

            const phos::color::PaletteInstanceId pid = cs.Palettes().RegisterDynamic(title, rgb);
            if (const phos::color::Palette* p = cs.Palettes().Get(pid))
            {
                st.palette_ref = p->ref;
                // UI palette selection: follow the core palette identity.
                st.ui_palette_ref = st.palette_ref;
            }
            else
            {
                st.palette_ref.is_builtin = true;
                st.palette_ref.builtin = phos::color::BuiltinPalette::Vga16;
                // UI palette selection: follow the core palette identity.
                st.ui_palette_ref = st.palette_ref;
            }
        }
        else
        {
            // No palette chunk => default VGA16.
            st.palette_ref.is_builtin = true;
            st.palette_ref.builtin = phos::color::BuiltinPalette::Vga16;
            // UI palette selection: follow the core palette identity.
            st.ui_palette_ref = st.palette_ref;
        }

        // IMPORTANT: snapshot fields drive rendering; keep them in sync.
        st.current.palette_ref = st.palette_ref;
        st.current.ui_palette_ref = st.ui_palette_ref;
    }

    // Default bold semantics for XBin:
    // XBin is a classic bitmap-font workflow; treat bold as ANSI intensity by default.
    st.bold_semantics = (st.palette_ref.is_builtin && st.palette_ref.builtin == phos::color::BuiltinPalette::Vga16)
                            ? (int)AnsiCanvas::BoldSemantics::AnsiBright
                            : (int)AnsiCanvas::BoldSemantics::Typographic;

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

    // Embedded font is now attached via ProjectState.embedded_font above.
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

    auto& cs = phos::color::GetColorSystem();
    const phos::color::QuantizePolicy qpol = phos::color::DefaultQuantizePolicy();

    // Export is index-native: remap from the canvas palette to the chosen XBin 16-color palette.
    phos::color::PaletteInstanceId src_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
        src_pal = *id;

    phos::color::PaletteInstanceId dst_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16);
    if (options.include_palette)
    {
        if (options.target_palette == ExportOptions::TargetPalette::Vga16)
        {
            dst_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);
        }
        else if (options.target_palette == ExportOptions::TargetPalette::Xterm16)
        {
            dst_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm16);
        }
        else if (options.target_palette == ExportOptions::TargetPalette::CanvasIf16)
        {
            const phos::color::Palette* p = cs.Palettes().Get(src_pal);
            if (p && p->rgb.size() == 16)
                dst_pal = src_pal;
        }
        else if (options.target_palette == ExportOptions::TargetPalette::Explicit)
        {
            auto id = cs.Palettes().Resolve(options.explicit_palette_ref);
            if (!id)
            {
                err = "XBin export: explicit_palette_ref does not resolve.";
                return false;
            }
            const phos::color::Palette* p = cs.Palettes().Get(*id);
            if (!p || p->rgb.size() != 16)
            {
                err = "XBin export: explicit palette must be exactly 16 colors.";
                return false;
            }
            dst_pal = *id;
        }
        // else: Xterm16 (default)
    }
    else
    {
        // No palette chunk => readers assume default palette; encode with xterm16.
        dst_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);
    }

    const auto remap_to_16 = cs.Luts().GetOrBuildRemap(cs.Palettes(), src_pal, dst_pal, qpol);

    auto remap_index_to_16 = [&](AnsiCanvas::ColorIndex16 idx, int fallback) -> int {
        if (idx == AnsiCanvas::kUnsetIndex16)
            return fallback;
        if (remap_to_16 && (size_t)idx < remap_to_16->remap.size())
            return (int)remap_to_16->remap[(size_t)idx];

        // Budget-pressure fallback: exact scan via packed color round-trip.
        const std::uint32_t c32 =
            phos::color::ColorOps::IndexToColor32(cs.Palettes(), src_pal, phos::color::ColorIndex{idx});
        const phos::color::ColorIndex di =
            phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_pal, c32, qpol);
        return di.IsUnset() ? fallback : (int)std::clamp<int>((int)di.v, 0, 15);
    };

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
        WritePaletteChunk(out_bytes, dst_pal);

    if (options.include_font)
        out_bytes.insert(out_bytes.end(), ef->bitmap.begin(), ef->bitmap.end());

    // Gather cell data and quantize to 16-color indices.
    std::vector<std::uint8_t> ch((size_t)cols * (size_t)rows);
    std::vector<std::uint8_t> at((size_t)cols * (size_t)rows);

    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            phos::GlyphId glyph = phos::glyph::MakeUnicodeScalar(U' ');
            AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;

            if (options.source == ExportOptions::Source::Composite)
            {
                (void)canvas.GetCompositeCellPublicGlyphIndices(y, x, glyph, fg, bg);
            }
            else
            {
                const int li = canvas.GetActiveLayerIndex();
                glyph = (phos::GlyphId)canvas.GetLayerGlyph(li, y, x);
                (void)canvas.GetLayerCellIndices(li, y, x, fg, bg);
            }

            // Unset -> default indices (classic XBin expectation).
            int fg_i = remap_index_to_16(fg, 7);
            int bg_i = remap_index_to_16(bg, 0);

            fg_i = std::clamp(fg_i, 0, 15);
            bg_i = std::clamp(bg_i, 0, 15);

            const size_t idx = (size_t)y * (size_t)cols + (size_t)x;
            const phos::glyph::Kind k = phos::glyph::GetKind(glyph);
            const char32_t rep_cp = phos::glyph::ToUnicodeRepresentative(glyph);

            if (embedded_font)
            {
                // Prefer a real embedded index (token or legacy PUA) if available.
                if (auto ei = phos::glyph::TryGetEmbeddedIndex(glyph, ef))
                    ch[idx] = (std::uint8_t)std::clamp((int)*ei, 0, 255);
                else if (k == phos::glyph::Kind::BitmapIndex)
                    ch[idx] = (std::uint8_t)std::clamp((int)phos::glyph::BitmapIndexValue(glyph), 0, 255);
                else
                    ch[idx] = UnicodeToCp437Byte(rep_cp);
            }
            else
            {
                // Non-embedded export: preserve direct indices when present.
                if (k == phos::glyph::Kind::BitmapIndex)
                    ch[idx] = (std::uint8_t)std::clamp((int)phos::glyph::BitmapIndexValue(glyph), 0, 255);
                else if (k == phos::glyph::Kind::EmbeddedIndex)
                    ch[idx] = (std::uint8_t)std::clamp((int)phos::glyph::EmbeddedIndexValue(glyph), 0, 255);
                else
                    ch[idx] = UnicodeToCp437Byte(rep_cp);
            }

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


