#include "io/formats/ansi.h"

#include "core/color_system.h"
#include "core/fonts.h"
#include "core/glyph_resolve.h"
#include "core/paths.h"
#include "core/xterm256_palette.h"
#include "io/formats/sauce.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <string_view>
#include <vector>

namespace formats
{
namespace ansi
{
const std::vector<std::string_view>& ImportExtensions()
{
    // Lowercase, no leading dots.
    // Keep this list focused on extensions commonly treated as ANSI/textmode payloads.
    // (Plaintext-intent extensions like .txt/.asc are handled by formats::plaintext.)
    static const std::vector<std::string_view> exts = {"ans", "nfo", "diz"};
    return exts;
}

const std::vector<std::string_view>& ExportExtensions()
{
    // Lowercase, no leading dots.
    static const std::vector<std::string_view> exts = {"ans"};
    return exts;
}

namespace
{
static constexpr std::uint8_t LF  = '\n';
static constexpr std::uint8_t CR  = '\r';
static constexpr std::uint8_t TAB = '\t';
static constexpr std::uint8_t SUB = 26;
static constexpr std::uint8_t ESC = 27;

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

struct PaletteDef32
{
    std::string title;
    std::vector<AnsiCanvas::Color32> colors;
};

// Canonical ANSI art palette: VGA 16 (matches assets/color-palettes.json "VGA 16").
// IMPORTANT: Indices here are **ANSI/SGR order**, not IBM PC attribute order:
//   0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white
// and 8..15 are the bright variants.
struct VgaRgb { std::uint8_t r, g, b; };
static constexpr VgaRgb kVga16[16] = {
    {0x00, 0x00, 0x00}, // 0 black
    {0xAA, 0x00, 0x00}, // 1 red
    {0x00, 0xAA, 0x00}, // 2 green
    {0xAA, 0x55, 0x00}, // 3 yellow/brown
    {0x00, 0x00, 0xAA}, // 4 blue
    {0xAA, 0x00, 0xAA}, // 5 magenta
    {0x00, 0xAA, 0xAA}, // 6 cyan
    {0xAA, 0xAA, 0xAA}, // 7 light gray ("white" in classic 8-color)
    {0x55, 0x55, 0x55}, // 8 dark gray
    {0xFF, 0x55, 0x55}, // 9 bright red
    {0x55, 0xFF, 0x55}, // 10 bright green
    {0xFF, 0xFF, 0x55}, // 11 bright yellow
    {0x55, 0x55, 0xFF}, // 12 bright blue
    {0xFF, 0x55, 0xFF}, // 13 bright magenta
    {0x55, 0xFF, 0xFF}, // 14 bright cyan
    {0xFF, 0xFF, 0xFF}, // 15 bright white
};

static inline AnsiCanvas::Color32 Vga16Color32ForIndex(int idx)
{
    idx = std::clamp(idx, 0, 15);
    const VgaRgb c = kVga16[idx];
    return PackImGuiCol32(c.r, c.g, c.b);
}

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

    // Our packed colors follow ImGui's IM_COL32 (ABGR).
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

static std::string InferPaletteTitleFromHistogram(const std::unordered_map<AnsiCanvas::Color32, std::uint32_t>& hist,
                                                  const std::vector<PaletteDef32>& palettes)
{
    if (hist.empty() || palettes.empty())
        return {};

    // Prefer the smallest palette that exactly contains all used colors.
    // This prevents giant supersets (e.g. "Xterm 256") from beating tight palettes (e.g. "VGA 16")
    // when the artwork is clearly limited to a small, exact set.
    {
        std::vector<size_t> order;
        order.reserve(palettes.size());
        for (size_t i = 0; i < palettes.size(); ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto& pa = palettes[a];
            const auto& pb = palettes[b];
            if (pa.colors.size() != pb.colors.size())
                return pa.colors.size() < pb.colors.size();
            return pa.title < pb.title;
        });

        for (size_t idx : order)
        {
            const auto& p = palettes[idx];
            if (p.colors.empty())
                continue;
            std::unordered_set<AnsiCanvas::Color32> s;
            s.reserve(p.colors.size());
            for (const auto c : p.colors)
                s.insert(c);

            bool ok = true;
            for (const auto& kv : hist)
            {
                if (s.find(kv.first) == s.end())
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return p.title;
        }
    }

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

    std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
    std::string best_title;

    for (const auto& p : palettes)
    {
        if (p.colors.empty())
            continue;

        std::uint64_t score = 0;
        for (const auto& kv : hist)
        {
            const AnsiCanvas::Color32 used = kv.first;
            const std::uint32_t count = kv.second;

            std::uint32_t best_d2 = std::numeric_limits<std::uint32_t>::max();
            for (AnsiCanvas::Color32 pc : p.colors)
                best_d2 = std::min(best_d2, dist2_rgb(used, pc));
            score += (std::uint64_t)best_d2 * (std::uint64_t)count;

            // Early exit if already worse.
            if (score >= best_score)
                break;
        }

        // Small bias toward smaller palettes to avoid "superset wins" when scores are similar.
        // (This is only used if there was no exact match above.)
        score += (std::uint64_t)p.colors.size();

        if (score < best_score)
        {
            best_score = score;
            best_title = p.title;
        }
    }

    return best_title;
}

static void Utf8Append(char32_t cp, std::vector<std::uint8_t>& out)
{
    if (cp <= 0x7F)
    {
        out.push_back((std::uint8_t)cp);
        return;
    }
    if (cp <= 0x7FF)
    {
        out.push_back((std::uint8_t)(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back((std::uint8_t)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp <= 0xFFFF)
    {
        out.push_back((std::uint8_t)(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back((std::uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((std::uint8_t)(0x80 | (cp & 0x3F)));
        return;
    }
    out.push_back((std::uint8_t)(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back((std::uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((std::uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((std::uint8_t)(0x80 | (cp & 0x3F)));
}

static std::uint8_t UnicodeToByteOrFallback(phos::encodings::EncodingId enc, char32_t cp, std::uint8_t fallback)
{
    std::uint8_t b = fallback;
    if (phos::encodings::UnicodeToByte(enc, cp, b))
        return b;
    return fallback;
}

static void EmitCsi(std::vector<std::uint8_t>& out, std::string_view body, char final_byte)
{
    out.push_back(ESC);
    out.push_back((std::uint8_t)'[');
    out.insert(out.end(), body.begin(), body.end());
    out.push_back((std::uint8_t)final_byte);
}

static void EmitSgr(std::vector<std::uint8_t>& out, std::string_view params)
{
    EmitCsi(out, params, 'm');
}

static int Digits10(int v)
{
    if (v < 10) return 1;
    if (v < 100) return 2;
    if (v < 1000) return 3;
    if (v < 10000) return 4;
    if (v < 100000) return 5;
    if (v < 1000000) return 6;
    return 7;
}

static bool IsBlankish(phos::GlyphId g)
{
    if (g == 0)
        return true;
    return phos::glyph::IsBlank(g);
}

static AnsiCanvas::Color32 DefaultFgForExport(const ExportOptions& opt)
{
    if (opt.default_fg != 0)
        return opt.default_fg;
    return (AnsiCanvas::Color32)xterm256::Color32ForIndex(7);
}

static AnsiCanvas::Color32 DefaultBgForExport(const ExportOptions& opt)
{
    if (opt.default_bg != 0)
        return opt.default_bg;
    return (AnsiCanvas::Color32)xterm256::Color32ForIndex(0);
}

struct ExportCell
{
    phos::GlyphId glyph = phos::glyph::MakeUnicodeScalar(U' ');
    char32_t      cp = U' '; // Unicode representative (used for UTF-8 output / fallbacks)

    // Index-native channels (Phase B): palette indices in the canvas's active palette.
    // Unset is represented as kUnsetIndex16.
    AnsiCanvas::ColorIndex16 fg_idx = AnsiCanvas::kUnsetIndex16;
    AnsiCanvas::ColorIndex16 bg_idx = AnsiCanvas::kUnsetIndex16;

    // Packed-color channels are kept for output modes that require RGB (truecolor / Pablo `...t`).
    // 0 means "unset".
    AnsiCanvas::Color32 fg = 0;
    AnsiCanvas::Color32 bg = 0;

    AnsiCanvas::Attrs attrs = 0;
};

static bool SampleCell(const AnsiCanvas& canvas, const ExportOptions& opt, int row, int col, ExportCell& out)
{
    phos::GlyphId glyph = phos::glyph::MakeUnicodeScalar(U' ');
    AnsiCanvas::ColorIndex16 fg_idx = AnsiCanvas::kUnsetIndex16;
    AnsiCanvas::ColorIndex16 bg_idx = AnsiCanvas::kUnsetIndex16;
    AnsiCanvas::Attrs attrs = 0;

    if (opt.source == ExportOptions::Source::Composite)
    {
        // GlyphId-native sampling (lossless token surface).
        if (!canvas.GetCompositeCellPublicGlyphIndices(row, col, glyph, fg_idx, bg_idx, attrs))
            return false;
    }
    else
    {
        const int layer = canvas.GetActiveLayerIndex();
        glyph = canvas.GetLayerGlyph(layer, row, col);
        (void)canvas.GetLayerCellIndices(layer, row, col, fg_idx, bg_idx);
        (void)canvas.GetLayerCellAttrs(layer, row, col, attrs);
    }

    // Packed-color bridge: only needed at truecolor output boundaries.
    AnsiCanvas::Color32 fg = 0;
    AnsiCanvas::Color32 bg = 0;
    if (opt.color_mode == ExportOptions::ColorMode::TrueColorSgr ||
        opt.color_mode == ExportOptions::ColorMode::TrueColorPabloT)
    {
        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
            pal = *id;
        if (fg_idx != AnsiCanvas::kUnsetIndex16)
            fg = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal, phos::color::ColorIndex{fg_idx});
        if (bg_idx != AnsiCanvas::kUnsetIndex16)
            bg = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal, phos::color::ColorIndex{bg_idx});
    }

    out.glyph = glyph;
    out.cp = phos::glyph::ToUnicodeRepresentative(glyph);
    out.fg_idx = fg_idx;
    out.bg_idx = bg_idx;
    out.fg = fg;
    out.bg = bg;
    out.attrs = attrs;
    return true;
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

[[maybe_unused]] static bool ContainsEsc(const std::vector<std::uint8_t>& bytes)
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

static std::vector<std::uint8_t> ExtractTextBytesIgnoringAnsi(const std::vector<std::uint8_t>& bytes, size_t parse_len)
{
    // Extracts "likely text payload" bytes for encoding heuristics by stripping common ANSI
    // sequences. This lets us detect UTF-8 content even when ESC sequences are present.
    //
    // We remove:
    // - CSI sequences: ESC [ ... <final>
    // We also skip SUB (0x1A) as end-of-stream marker.
    //
    // We keep:
    // - printable ASCII (>= 0x20)
    // - all bytes >= 0x80 (these carry the signal for UTF-8 vs CP437)
    std::vector<std::uint8_t> out;
    out.reserve(std::min<size_t>(parse_len, 1u << 20)); // cap reserve to avoid huge spikes

    size_t i = 0;
    static constexpr size_t kSeqMaxLen = 64;
    while (i < parse_len)
    {
        const std::uint8_t b = bytes[i];
        if (b == SUB)
            break;
        if (b != ESC)
        {
            if (b >= 0x20u || b >= 0x80u)
                out.push_back(b);
            i += 1;
            continue;
        }
        // ESC
        if (i + 1 < parse_len && bytes[i + 1] == (std::uint8_t)'[')
        {
            // CSI: skip until final byte.
            size_t j = i + 2;
            size_t consumed = 0;
            while (j < parse_len && consumed < kSeqMaxLen)
            {
                const char ch = (char)bytes[j];
                if (((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7E) || ch == '!')
                {
                    j += 1;
                    break;
                }
                j += 1;
                consumed += 1;
            }
            i = j;
            continue;
        }
        // Unknown ESC sequence: skip ESC itself; keep following bytes as potential text.
        i += 1;
    }
    return out;
}

static bool ShouldDecodeAsUtf8(const ImportOptions& options,
                               const std::vector<std::uint8_t>& bytes,
                               size_t parse_len,
                               const sauce::Parsed* sauce_parsed,
                               const fonts::FontId* sauce_font)
{
    // Auto-detect UTF-8 ANSI art vs classic CP437 ANSI art.
    //
    // Historically, this code treated "ESC present" as a signal for CP437, but modern ANSI
    // streams often embed ESC sequences with UTF-8 glyph payloads. We therefore run a UTF-8
    // validity heuristic on the *text payload bytes* (ANSI sequences stripped).
    if (!options.cp437)
        return true; // caller forced UTF-8

    const auto text_bytes = ExtractTextBytesIgnoringAnsi(bytes, parse_len);
    // Strong explicit signal: UTF-8 BOM in the text payload.
    if (text_bytes.size() >= 3 &&
        text_bytes[0] == 0xEFu && text_bytes[1] == 0xBBu && text_bytes[2] == 0xBFu)
    {
        return true;
    }

    // SAUCE hint: `data_type` describes the *kind of payload* (stream vs binary screen dump vs XBin),
    // not the character encoding, but it is still a strong signal for whether UTF-8 makes sense.
    //
    // - BinaryText (raw char/attr pairs) is inherently 8-bit.
    // - XBin should be routed to the XBin importer; if it reaches here, avoid treating it as UTF-8.
    if (sauce_parsed && sauce_parsed->record.present)
    {
        const std::uint8_t dt = sauce_parsed->record.data_type;
        if (dt == (std::uint8_t)sauce::DataType::BinaryText ||
            dt == (std::uint8_t)sauce::DataType::XBin)
        {
            return false;
        }
    }

    // SAUCE-first policy:
    // If the file declares a known font, respect it for text decoding decisions.
    //
    // - ImGuiAtlas fonts (e.g. Unscii) imply Unicode/UTF-8 text payload semantics.
    // - Bitmap fonts imply classic 8-bit byte semantics; do not auto-switch to UTF-8
    //   on heuristic signal alone (BOM remains an override).
    (void)sauce_parsed;
    if (sauce_font)
    {
        const fonts::FontInfo& finfo = fonts::Get(*sauce_font);
        if (finfo.kind == fonts::Kind::ImGuiAtlas)
            return true;
        if (finfo.kind == fonts::Kind::Bitmap1bpp)
            return false;
    }

    return LooksLikeUtf8Text(text_bytes);
}

static void GetSauceDimensions(const sauce::Parsed& sp, int& out_cols, int& out_rows)
{
    out_cols = 0;
    out_rows = 0;
    if (!sp.record.present)
        return;

    const std::uint8_t dt = sp.record.data_type;
    if (dt == (std::uint8_t)sauce::DataType::BinaryText)
    {
        // For BinaryText, SAUCE stores width in FileType as "half the width" (even widths only).
        const int cols = (int)sp.record.file_type * 2;
        if (cols > 0)
        {
            out_cols = cols;
            // Best-effort: infer height from payload length (char/attr pairs).
            const size_t bytes_per_row = (size_t)cols * 2;
            if (bytes_per_row > 0)
                out_rows = (int)(sp.payload_size / bytes_per_row);
        }
        return;
    }

    if (dt == (std::uint8_t)sauce::DataType::Character || dt == (std::uint8_t)sauce::DataType::XBin)
    {
        out_cols = (int)sp.record.tinfo1;
        out_rows = (int)sp.record.tinfo2;
        return;
    }
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
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool blink = false;      // SGR 5 (may be treated as iCE bright background depending on ImportOptions)
    bool invert = false;     // SGR 7
    bool strike = false;     // SGR 9

    // Import-time latches for DOS/scene conventions:
    // - In classic ANSI art, SGR 1 is commonly used to select "bright" ANSI16 foregrounds.
    // - With iCE colors enabled, SGR 5 is commonly used as a latch for "bright background"
    //   (not actual blinking text).
    //
    // We track these so we can correctly apply them across subsequent color changes,
    // and so we can undo the brightness bump on the corresponding reset codes.
    bool fg_bright_from_bold = false; // whether current fg_idx was bumped (+8) due to SGR 1 convention
    bool ice_bg = false;              // whether SGR 5 iCE bright-bg latch is active
    bool bg_bright_from_ice = false;  // whether current bg_idx was bumped (+8) due to iCE latch

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
    // VGA 16 palette (matches assets/color-palettes.json "VGA 16").
    // Stored as packed ABGR (ImGui IM_COL32-compatible) with alpha=255.
    idx = std::clamp(idx, 0, 15);
    switch (idx)
    {
        case 0:  return PackImGuiCol32(0x00, 0x00, 0x00);
        case 1:  return PackImGuiCol32(0xAA, 0x00, 0x00);
        case 2:  return PackImGuiCol32(0x00, 0xAA, 0x00);
        case 3:  return PackImGuiCol32(0xAA, 0x55, 0x00);
        case 4:  return PackImGuiCol32(0x00, 0x00, 0xAA);
        case 5:  return PackImGuiCol32(0xAA, 0x00, 0xAA);
        case 6:  return PackImGuiCol32(0x00, 0xAA, 0xAA);
        case 7:  return PackImGuiCol32(0xAA, 0xAA, 0xAA);
        case 8:  return PackImGuiCol32(0x55, 0x55, 0x55);
        case 9:  return PackImGuiCol32(0xFF, 0x55, 0x55);
        case 10: return PackImGuiCol32(0x55, 0xFF, 0x55);
        case 11: return PackImGuiCol32(0xFF, 0xFF, 0x55);
        case 12: return PackImGuiCol32(0x55, 0x55, 0xFF);
        case 13: return PackImGuiCol32(0xFF, 0x55, 0xFF);
        case 14: return PackImGuiCol32(0x55, 0xFF, 0xFF);
        default: return PackImGuiCol32(0xFF, 0xFF, 0xFF);
    }
}

static inline void ApplyDefaults(const ImportOptions& opt, Pen& pen)
{
    pen.bold = false;
    pen.dim = false;
    pen.italic = false;
    pen.underline = false;
    pen.blink = false;
    pen.invert = false;
    pen.strike = false;
    pen.fg_bright_from_bold = false;
    pen.ice_bg = false;
    pen.bg_bright_from_ice = false;

    pen.fg_mode = Mode::Palette16;
    pen.bg_mode = Mode::Palette16;
    pen.fg_idx = 7;
    pen.bg_idx = 0;

    const AnsiCanvas::Color32 def_fg = (opt.default_fg != 0) ? opt.default_fg : ColorFromAnsi16(7);
    const AnsiCanvas::Color32 def_bg = opt.default_bg_unset ? 0
                              : ((opt.default_bg != 0) ? opt.default_bg : ColorFromAnsi16(0));
    pen.fg = def_fg;
    pen.bg = def_bg;
}

static inline char32_t DecodeTextCp(const ImportOptions& opt, const std::vector<std::uint8_t>& bytes, size_t& i)
{
    const std::uint8_t b = bytes[i];
    i += 1;
    // Many ANSI art tools emit NUL bytes for "blank"; treat as space.
    // Also treat other control bytes (0x01..0x1F) as spaces to avoid injecting
    // "control glyphs" into modern Unicode fonts.
    if (b < 0x20u)
        return U' ';
    return phos::encodings::ByteToUnicode(opt.byte_encoding, b);
}

static inline bool DecodeTextUtf8(const ImportOptions& opt, const std::vector<std::uint8_t>& bytes, size_t& i, char32_t& out_cp)
{
    (void)opt;
    size_t before = i;
    if (DecodeOneUtf8(bytes.data(), bytes.size(), i, out_cp))
        return true;
    i = before + 1;
    out_cp = U'\uFFFD';
    return false;
}

static inline bool IsValidColumns(int cols)
{
    return cols >= 1 && cols <= 4096;
}

static inline int NormalizeInferredColumns(int cols)
{
    // Preserve long-standing UX expectations: don't auto-infer widths below 80 unless the
    // user explicitly forces it. For wider art, snap up to common terminal widths.
    cols = ClampColumns(cols);
    if (cols <= 80)
        return 80;
    if (cols <= 100)
        return 100;
    if (cols <= 132)
        return 132;
    if (cols <= 160)
        return 160;
    return cols;
}

static int MaxExplicitColumn1BasedFromCsi(const std::vector<std::uint8_t>& bytes, size_t parse_len)
{
    // Scans for CSI cursor positioning that explicitly references a column.
    // - CUP/HVP: ESC [ row ; col H/f  (1-based)
    // - CHA:     ESC [ col G          (1-based)
    //
    // This is a strong signal for intended width, especially for wrap-free positioning.
    int max_col_1 = 0;
    size_t i = 0;
    while (i < parse_len)
    {
        const std::uint8_t b = bytes[i];
        if (b != ESC)
        {
            i += 1;
            continue;
        }
        if (i + 1 >= parse_len || bytes[i + 1] != (std::uint8_t)'[')
        {
            i += 1;
            continue;
        }
        const size_t seq_start = i + 2; // after ESC[
        size_t j = seq_start;
        size_t consumed = 0;
        static constexpr size_t kSeqMaxLen = 64;
        char final = '\0';
        while (j < parse_len && consumed < kSeqMaxLen)
        {
            const char ch = (char)bytes[j];
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
            i += 1;
            continue;
        }

        const std::string_view params_view(reinterpret_cast<const char*>(bytes.data() + seq_start), j - seq_start);
        std::vector<int> params;
        ParseParams(params_view, params);
        auto param = [&](size_t idx, int def) -> int {
            if (idx >= params.size()) return def;
            return params[idx];
        };

        if (final == 'H' || final == 'f')
        {
            const int c1 = param(1, 1);
            if (c1 > max_col_1)
                max_col_1 = c1;
        }
        else if (final == 'G')
        {
            const int c1 = param(0, 1);
            if (c1 > max_col_1)
                max_col_1 = c1;
        }

        // Advance past the whole CSI.
        i = (j < parse_len) ? (j + 1) : parse_len;
    }
    return max_col_1;
}

static int MaxColumnUsedWithNewlines(const std::vector<std::uint8_t>& bytes,
                                     size_t parse_len,
                                     const ImportOptions& options,
                                     const sauce::Parsed* sauce_parsed,
                                     const fonts::FontId* sauce_font)
{
    // Conservative width inference for newline-delimited content:
    // simulate cursor positioning + printing without wrapping (i.e. "infinite width"),
    // and record the maximum column reached in any row.
    //
    // This handles mixed CR/LF, TABs, and common CSI cursor motions.
    int row = 0;
    int col = 0;
    int saved_row = 0;
    int saved_col = 0;
    // We intentionally ignore trailing padding spaces when inferring width from newline-delimited content.
    // Many ANSI exporters pad lines with spaces to a working width, but those spaces should not force a
    // wider canvas when importing into an editor.
    int max_last_non_space_col0 = -1; // maximum "last non-space" column across rows (0-based)
    int line_last_non_space_col0 = -1;

    // Use same text decoding mode decision as the importer, because UTF-8 may consume
    // multiple bytes per displayed column.
    bool decode_cp437 = options.cp437;
    if (ShouldDecodeAsUtf8(options, bytes, parse_len, sauce_parsed, sauce_font))
        decode_cp437 = false;

    enum class State { Text, Sequence, End };
    State state = State::Text;
    size_t i = 0;
    static constexpr size_t kSeqMaxLen = 64;

    while (i < parse_len && state != State::End)
    {
        const std::uint8_t b = bytes[i];
        if (state == State::Text)
        {
            switch (b)
            {
                case LF:
                    row += 1;
                    if (line_last_non_space_col0 > max_last_non_space_col0)
                        max_last_non_space_col0 = line_last_non_space_col0;
                    line_last_non_space_col0 = -1;
                    col = 0;
                    i += 1;
                    break;
                case CR:
                    col = 0;
                    i += 1;
                    break;
                case TAB:
                {
                    const int tab_w = 8;
                    const int next = ((col / tab_w) + 1) * tab_w;
                    col = next;
                    i += 1;
                    break;
                }
                case SUB:
                    state = State::End;
                    break;
                case ESC:
                    if (i + 1 < parse_len && bytes[i + 1] == (std::uint8_t)'[')
                    {
                        state = State::Sequence;
                        i += 2;
                    }
                    else
                    {
                        i += 1;
                    }
                    break;
                default:
                {
                    char32_t cp = U'\0';
                    if (decode_cp437)
                        cp = DecodeTextCp(options, bytes, i);
                    else
                        DecodeTextUtf8(options, bytes, i, cp);

                    // Mirror importer behavior: in CP437 mode, control bytes can map to glyphs/spaces;
                    // in UTF-8 mode, ASCII control codes are treated as non-printing.
                    if (decode_cp437 || cp >= 0x20)
                    {
                        if (cp != U' ')
                            line_last_non_space_col0 = std::max(line_last_non_space_col0, col);
                        col += 1;
                    }
                    break;
                }
            }
            continue;
        }

        // CSI sequence parsing (same terminator rules as importer).
        if (state == State::Sequence)
        {
            const size_t seq_start = i;
            size_t j = i;
            size_t consumed = 0;
            char final = '\0';
            while (j < parse_len && consumed < kSeqMaxLen)
            {
                const char ch = (char)bytes[j];
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
                state = State::Text;
                i = std::min(parse_len, seq_start + consumed + 1);
                continue;
            }

            const std::string_view params_view(reinterpret_cast<const char*>(bytes.data() + seq_start), j - seq_start);
            std::vector<int> params;
            ParseParams(params_view, params);
            auto param = [&](size_t idx, int def) -> int {
                if (idx >= params.size()) return def;
                return params[idx];
            };

            if (final == 'H' || final == 'f')
            {
                const int r1 = param(0, 1);
                const int c1 = param(1, 1);
                row = std::max(0, (r1 ? r1 : 1) - 1);
                col = std::max(0, (c1 ? c1 : 1) - 1);
            }
            else if (final == 'A')
            {
                const int n = param(0, 0);
                row -= (n ? n : 1);
                if (row < 0) row = 0;
            }
            else if (final == 'B')
            {
                const int n = param(0, 0);
                row += (n ? n : 1);
            }
            else if (final == 'C')
            {
                const int n = param(0, 0);
                col += (n ? n : 1);
            }
            else if (final == 'D')
            {
                const int n = param(0, 0);
                col -= (n ? n : 1);
                if (col < 0) col = 0;
            }
            else if (final == 'G')
            {
                const int c1 = param(0, 1);
                col = std::max(0, (c1 ? c1 : 1) - 1);
            }
            else if (final == 's')
            {
                saved_row = row;
                saved_col = col;
            }
            else if (final == 'u')
            {
                row = saved_row;
                col = saved_col;
            }

            state = State::Text;
            i = (j < parse_len) ? (j + 1) : parse_len;
            continue;
        }
    }

    // Commit final line (if any).
    if (line_last_non_space_col0 > max_last_non_space_col0)
        max_last_non_space_col0 = line_last_non_space_col0;
    return max_last_non_space_col0;
}

static int DetermineAutoColumns(const std::vector<std::uint8_t>& bytes,
                                size_t parse_len,
                                const sauce::Parsed& sp,
                                const ImportOptions& options)
{
    std::optional<fonts::FontId> sauce_font;
    if (sp.record.present)
    {
        fonts::FontId fid;
        if (fonts::TryFromSauceName(sp.record.tinfos, fid))
            sauce_font = fid;
    }

    int sauce_cols = 0;
    int sauce_rows = 0;
    GetSauceDimensions(sp, sauce_cols, sauce_rows);
    if (IsValidColumns(sauce_cols))
        return NormalizeInferredColumns(sauce_cols);

    // Strong signal: explicit cursor positioning to a column.
    const int max_col_1 = MaxExplicitColumn1BasedFromCsi(bytes, parse_len);
    if (max_col_1 > 0)
        return NormalizeInferredColumns(max_col_1);

    // Newline-delimited content: infer from maximum used column without wrapping.
    bool has_newlines = false;
    for (size_t i = 0; i < parse_len; ++i)
    {
        const std::uint8_t b = bytes[i];
        if (b == LF || b == CR)
        {
            has_newlines = true;
            break;
        }
    }
    if (has_newlines)
    {
        const int max_col0 = MaxColumnUsedWithNewlines(bytes, parse_len, options, &sp, sauce_font ? &(*sauce_font) : nullptr);
        const int cols = (max_col0 >= 0) ? (max_col0 + 1) : 1;
        return NormalizeInferredColumns(cols);
    }

    // Wrap-only content without SAUCE or explicit cursor width is inherently ambiguous.
    // Keep legacy behavior as the last resort.
    return 80;
}
} // namespace

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options)
{
    err.clear();

    // Guard: if the payload looks like an XBin file, fail fast so callers can route to the XBin importer.
    // XBin starts with "XBIN" + 0x1A, which would otherwise be interpreted as an ANSI SUB (end).
    if (bytes.size() >= 5 &&
        bytes[0] == (std::uint8_t)'X' && bytes[1] == (std::uint8_t)'B' &&
        bytes[2] == (std::uint8_t)'I' && bytes[3] == (std::uint8_t)'N' &&
        bytes[4] == (std::uint8_t)0x1A)
    {
        err = "File appears to be XBin (XBIN header). Use the XBin (.xb) importer.";
        return false;
    }

    // Auto-width + SAUCE detection (SAUCE fields are spec'd as CP437).
    sauce::Parsed sp;
    std::string serr;
    (void)sauce::ParseFromBytes(bytes, sp, serr, true /* SAUCE fields are spec'd as CP437 */);
    const size_t parse_len = sp.record.present ? std::min(sp.payload_size, bytes.size()) : bytes.size();

    // Automatic import policy (no UI / no persisted knobs):
    // If SAUCE declares a known font (tinfos), use that to pick the byte encoding table for 8-bit mode.
    // This lets the file itself inform byte->Unicode semantics when we need them.
    ImportOptions opt = options;
    std::optional<fonts::FontId> sauce_font;
    if (sp.record.present)
    {
        fonts::FontId fid;
        if (fonts::TryFromSauceName(sp.record.tinfos, fid))
        {
            opt.byte_encoding = fonts::EncodingForFont(fid);
            sauce_font = fid;
        }
    }

    int columns = 80;
    if (opt.columns > 0)
    {
        columns = ClampColumns(opt.columns);
    }
    else
    {
        columns = ClampColumns(DetermineAutoColumns(bytes, parse_len, sp, opt));
    }
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
    ApplyDefaults(opt, pen);
    bool saw_xterm256 = false;
    bool saw_truecolor = false;

    // Auto-detect UTF-8 ANSI art vs classic CP437 ANSI art.
    bool decode_cp437 = opt.cp437;
    if (ShouldDecodeAsUtf8(opt, bytes, parse_len, &sp, sauce_font ? &(*sauce_font) : nullptr))
        decode_cp437 = false;

    // We build a single layer (Base).
    const phos::GlyphId blank_glyph =
        (decode_cp437 && opt.glyph_bytes_policy == ImportOptions::GlyphBytesPolicy::StoreAsBitmapIndex)
            ? phos::glyph::MakeBitmapIndex((std::uint16_t)' ')
            : phos::glyph::MakeUnicodeScalar(U' ');

    std::vector<phos::GlyphId> glyph_plane;
    std::vector<AnsiCanvas::Color32> fg32;
    std::vector<AnsiCanvas::Color32> bg32;
    std::vector<AnsiCanvas::Attrs> attrs;

    auto ensure_rows = [&](int rows_needed)
    {
        if (rows_needed < 1) rows_needed = 1;
        const size_t need = (size_t)rows_needed * (size_t)columns;
        if (glyph_plane.size() < need)
        {
            glyph_plane.resize(need, blank_glyph);
            fg32.resize(need, 0);
            bg32.resize(need, pen.bg); // default background
            attrs.resize(need, 0);
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

    auto put_glyph = [&](phos::GlyphId g)
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

        glyph_plane[at] = g;
        fg32[at] = pen.fg;
        bg32[at] = pen.bg;

        AnsiCanvas::Attrs a = 0;
        if (pen.bold)      a |= AnsiCanvas::Attr_Bold;
        if (pen.dim)       a |= AnsiCanvas::Attr_Dim;
        if (pen.italic)    a |= AnsiCanvas::Attr_Italic;
        if (pen.underline) a |= AnsiCanvas::Attr_Underline;
        if (pen.blink)     a |= AnsiCanvas::Attr_Blink;
        if (pen.invert)    a |= AnsiCanvas::Attr_Reverse;
        if (pen.strike)    a |= AnsiCanvas::Attr_Strikethrough;
        attrs[at] = a;

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

    while (i < parse_len && state != State::End)
    {
        if (opt.wrap_policy == ImportOptions::WrapPolicy::LibAnsiLoveEager)
        {
            // libansilove wraps before processing the next character.
            //
            // However, for streams that include explicit newlines, wrapping *before* consuming
            // an LF/CR at the exact boundary can double-advance (blank lines). Avoid pre-wrap
            // when the next byte is a newline control.
            const std::uint8_t next_b = bytes[i];
            if (state == State::Text && col == columns && next_b != LF && next_b != CR)
            {
                row += 1;
                col = 0;
            }
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
                        put_glyph(blank_glyph);
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
                    if (decode_cp437)
                    {
                        const std::uint8_t raw = bytes[i];
                        // DecodeTextCp consumes one byte and applies our historical "control bytes => space" policy.
                        const char32_t cp = DecodeTextCp(opt, bytes, i);

                        // Byte-mode import can either decode to Unicode scalars (legacy) or preserve byte identity
                        // as BitmapIndex glyph tokens (lossless index workflows).
                        if (opt.glyph_bytes_policy == ImportOptions::GlyphBytesPolicy::StoreAsBitmapIndex)
                        {
                            const std::uint8_t b = (raw < 0x20u) ? (std::uint8_t)' ' : raw;
                            put_glyph(phos::glyph::MakeBitmapIndex((std::uint16_t)b));
                        }
                        else
                        {
                            put_glyph(phos::glyph::MakeUnicodeScalar(cp));
                        }
                    }
                    else
                    {
                        char32_t cp = U'\0';
                        DecodeTextUtf8(opt, bytes, i, cp);

                        // Skip a leading UTF-8 BOM if present (common in some modern ANSI exports).
                        // Treat it as a zero-width marker rather than a printable glyph.
                        if (row == 0 && col == 0 && cp == (char32_t)0xFEFF)
                            break;

                        // For UTF-8, treat ASCII control codes as non-printing.
                        if (cp >= 0x20)
                            put_glyph(phos::glyph::MakeUnicodeScalar(cp));
                    }
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
                    glyph_plane.assign((size_t)columns, blank_glyph);
                    fg32.assign((size_t)columns, 0);
                    bg32.assign((size_t)columns, pen.bg);
                    attrs.assign((size_t)columns, 0);
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
                            pen.fg_bright_from_bold = true;
                        }
                        pen.bold = true;
                    }
                    else if (code == 2)
                    {
                        pen.dim = true;
                    }
                    else if (code == 3)
                    {
                        pen.italic = true;
                    }
                    else if (code == 4)
                    {
                        pen.underline = true;
                    }
                    else if (code == 5)
                    {
                        // ICE colors: blink -> bright background latch (common ANSI art convention).
                        //
                        // In iCE mode, SGR 5 acts like a stateful "bright background" bit that should
                        // apply to subsequent 40-47 background color changes, not just the current color.
                        // We therefore track a latch (pen.ice_bg) and apply the +8 bump on bg updates.
                        if (options.icecolors && pen.bg_mode == Mode::Palette16)
                        {
                            pen.ice_bg = true;
                            if (pen.bg_idx >= 0 && pen.bg_idx < 8)
                            {
                                pen.bg_idx += 8;
                                pen.bg = ColorFromAnsi16(pen.bg_idx);
                                pen.bg_bright_from_ice = true;
                            }
                            else
                            {
                                // Background is already bright or non-standard; don't mark it as an iCE bump.
                                pen.bg_bright_from_ice = false;
                            }
                            // Keep pen.blink false here: iCE files shouldn't blink in the editor.
                            pen.blink = false;
                        }
                        else
                        {
                            // Non-iCE mode: treat as real blink attribute.
                            pen.blink = true;
                        }
                    }
                    else if (code == 7)
                    {
                        pen.invert = true;
                    }
                    else if (code == 9)
                    {
                        pen.strike = true;
                    }
                    else if (code == 27)
                    {
                        pen.invert = false;
                    }
                    else if (code == 22)
                    {
                        // Normal intensity: in classic ANSI16 convention, this can undo the SGR 1 "bright fg" bump.
                        if (pen.fg_bright_from_bold && pen.fg_mode == Mode::Palette16 && pen.fg_idx >= 8 && pen.fg_idx < 16)
                        {
                            pen.fg_idx -= 8;
                            pen.fg = ColorFromAnsi16(pen.fg_idx);
                        }
                        pen.bold = false;
                        pen.dim = false;
                        pen.fg_bright_from_bold = false;
                    }
                    else if (code == 23)
                    {
                        pen.italic = false;
                    }
                    else if (code == 24)
                    {
                        pen.underline = false;
                    }
                    else if (code == 25)
                    {
                        // Blink off. In iCE mode, this also disables the bright-background latch.
                        if (pen.ice_bg && options.icecolors)
                        {
                            pen.ice_bg = false;
                            if (pen.bg_bright_from_ice && pen.bg_mode == Mode::Palette16 && pen.bg_idx >= 8 && pen.bg_idx < 16)
                            {
                                pen.bg_idx -= 8;
                                pen.bg = ColorFromAnsi16(pen.bg_idx);
                            }
                            pen.bg_bright_from_ice = false;
                        }
                        pen.blink = false;
                    }
                    else if (code == 29)
                    {
                        pen.strike = false;
                    }
                    else if (code == 39)
                    {
                        // Reset fg to default.
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = 7;
                        pen.fg = (options.default_fg != 0) ? options.default_fg : ColorFromAnsi16(7);
                        pen.fg_bright_from_bold = false;
                    }
                    else if (code == 49)
                    {
                        // Reset bg to default.
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = 0;
                        pen.bg = options.default_bg_unset ? 0
                               : ((options.default_bg != 0) ? options.default_bg : ColorFromAnsi16(0));
                        // Keep iCE latch state, but this specific bg value is not an iCE bump (unset/default can be transparent).
                        pen.bg_bright_from_ice = false;
                    }
                    else if (code >= 30 && code <= 37)
                    {
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = code - 30;
                        if (pen.bold)
                        {
                            pen.fg_idx += 8;
                            pen.fg_bright_from_bold = true;
                        }
                        else
                        {
                            pen.fg_bright_from_bold = false;
                        }
                        pen.fg = ColorFromAnsi16(pen.fg_idx);
                    }
                    else if (code >= 90 && code <= 97)
                    {
                        pen.fg_mode = Mode::Palette16;
                        pen.fg_idx = (code - 90) + 8;
                        pen.fg = ColorFromAnsi16(pen.fg_idx);
                        pen.fg_bright_from_bold = false;
                    }
                    else if (code >= 40 && code <= 47)
                    {
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = code - 40;
                        if (pen.ice_bg && options.icecolors)
                        {
                            pen.bg_idx += 8;
                            pen.bg_bright_from_ice = true;
                        }
                        else
                        {
                            pen.bg_bright_from_ice = false;
                        }
                        pen.bg = ColorFromAnsi16(pen.bg_idx);
                    }
                    else if (code >= 100 && code <= 107)
                    {
                        pen.bg_mode = Mode::Palette16;
                        pen.bg_idx = (code - 100) + 8;
                        pen.bg = ColorFromAnsi16(pen.bg_idx);
                        pen.bg_bright_from_ice = false;
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
                                    pen.fg_bright_from_bold = false;
                                    saw_xterm256 = true;
                                }
                                else
                                {
                                    pen.bg_mode = Mode::Xterm256;
                                    pen.bg_idx = idx;
                                    pen.bg = col32;
                                    pen.bg_bright_from_ice = false;
                                    saw_xterm256 = true;
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
                                    pen.fg_bright_from_bold = false;
                                    saw_truecolor = true;
                                }
                                else
                                {
                                    pen.bg_mode = Mode::TrueColor;
                                    pen.bg = col32;
                                    pen.bg_bright_from_ice = false;
                                    saw_truecolor = true;
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
                        saw_truecolor = true;
                    }
                    else if (which == 1)
                    {
                        pen.fg_mode = Mode::TrueColor;
                        pen.fg = col32;
                        saw_truecolor = true;
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
    // Keep this state at the current in-memory schema version so GlyphId tokens remain meaningful.
    st.version = 11;
    st.undo_limit = 0; // unlimited by default
    st.current.columns = out_cols;
    st.current.rows = out_rows;
    st.current.active_layer = 0;
    st.current.caret_row = 0;
    st.current.caret_col = 0;
    st.current.layers.clear();
    st.current.layers.resize(1);
    st.current.layers[0].name = "Base";
    st.current.layers[0].visible = true;

    // Palette inference: scan all used fg/bg colors and pick the closest palette from assets/color-palettes.json.
    // This is a UI convenience (helps the colour picker default to something sensible) and does not affect stored colors.
    {
        std::unordered_map<AnsiCanvas::Color32, std::uint32_t> hist;
        hist.reserve(64);
        for (const auto c : fg32)
            if (c != 0) ++hist[c];
        for (const auto c : bg32)
            if (c != 0) ++hist[c];

        // Avoid guessing from near-empty art (e.g. mostly-unset imports).
        if (hist.size() >= 2)
        {
            std::vector<PaletteDef32> pals;
            std::string perr;
            const std::string pal_path = PhosphorAssetPath("color-palettes.json");
            if (LoadPalettesFromJson32(pal_path, pals, perr))
            {
                const std::string inferred = InferPaletteTitleFromHistogram(hist, pals);
                if (!inferred.empty())
                    st.colour_palette_title = inferred;
            }
        }
    }

    // Palette identity + indexed colors (Phase B).
    {
        auto& cs = phos::color::GetColorSystem();
        const phos::color::BuiltinPalette builtin =
            (saw_xterm256 || saw_truecolor) ? phos::color::BuiltinPalette::Xterm256 : phos::color::BuiltinPalette::Vga16;
        st.palette_ref.is_builtin = true;
        st.palette_ref.builtin = builtin;
        // IMPORTANT: Set both the project-level metadata and the active snapshot palette_ref.
        // SetProjectState() applies the snapshot palette_ref for rendering.
        st.current.palette_ref = st.palette_ref;
        st.current.colour_palette_title = st.colour_palette_title;

        const phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(builtin);
        const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();

        std::vector<AnsiCanvas::ColorIndex16> fg;
        std::vector<AnsiCanvas::ColorIndex16> bg;
        fg.resize(fg32.size(), AnsiCanvas::kUnsetIndex16);
        bg.resize(bg32.size(), AnsiCanvas::kUnsetIndex16);
        for (std::size_t i2 = 0; i2 < fg32.size(); ++i2)
        {
            const phos::color::ColorIndex idx = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal, (std::uint32_t)fg32[i2], qp);
            fg[i2] = idx.IsUnset() ? AnsiCanvas::kUnsetIndex16 : (AnsiCanvas::ColorIndex16)idx.v;
        }
        for (std::size_t i2 = 0; i2 < bg32.size(); ++i2)
        {
            const phos::color::ColorIndex idx = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal, (std::uint32_t)bg32[i2], qp);
            bg[i2] = idx.IsUnset() ? AnsiCanvas::kUnsetIndex16 : (AnsiCanvas::ColorIndex16)idx.v;
        }

        std::vector<AnsiCanvas::GlyphId> glyphs;
        glyphs = std::move(glyph_plane);
        st.current.layers[0].cells = std::move(glyphs);
        st.current.layers[0].fg = std::move(fg);
        st.current.layers[0].bg = std::move(bg);
    }
    st.current.layers[0].attrs = std::move(attrs);

    // Preserve SAUCE metadata (if present) in the project state so exports and .phos saves can reuse it.
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
        // No SAUCE record: choose a reasonable default font based on decoding mode so the
        // imported canvas renders as expected without requiring manual configuration.
        st.sauce.present = true;
        const fonts::FontId fid = decode_cp437 ? fonts::FontId::Font_PC_80x25 : fonts::FontId::Unscii;
        const std::string_view sname = fonts::ToSauceName(fid);
        if (!sname.empty())
            st.sauce.tinfos.assign(sname.begin(), sname.end());
    }

    AnsiCanvas canvas(out_cols);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply imported ANSI state." : apply_err;
        return false;
    }

    out_canvas = std::move(canvas);
    (void)colMax;
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

// ---------------------------------------------------------------------------
// Export implementation
// ---------------------------------------------------------------------------
bool ExportCanvasToBytes(const AnsiCanvas& canvas,
                         std::vector<std::uint8_t>& out_bytes,
                         std::string& err,
                         const ExportOptions& options)
{
    err.clear();
    out_bytes.clear();

    const int cols = std::max(1, canvas.GetColumns());
    const int rows = std::max(1, canvas.GetRows());

    // Optional BOM.
    if (options.text_encoding == ExportOptions::TextEncoding::Utf8Bom)
    {
        out_bytes.push_back(0xEF);
        out_bytes.push_back(0xBB);
        out_bytes.push_back(0xBF);
    }

    // Screen preparation.
    if (options.screen_prep == ExportOptions::ScreenPrep::ClearScreen || options.screen_prep == ExportOptions::ScreenPrep::ClearAndHome)
        EmitCsi(out_bytes, "2", 'J');
    if (options.screen_prep == ExportOptions::ScreenPrep::Home || options.screen_prep == ExportOptions::ScreenPrep::ClearAndHome)
        EmitCsi(out_bytes, "", 'H');

    // Track current "pen" state for SGR diffing.
    struct PenOut
    {
        bool bold = false;
        bool dim = false;
        bool italic = false;
        bool underline = false;
        bool blink = false;
        bool invert = false;
        bool strike = false;
        bool has_fg = false;
        bool has_bg = false;
        bool fg_tc = false; // whether last-set fg was via Pablo `...t` truecolor overlay
        bool bg_tc = false; // whether last-set bg was via Pablo `...t` truecolor overlay
        int  fg_idx = 7;
        int  bg_idx = 0;
        AnsiCanvas::Color32 fg = 0;
        AnsiCanvas::Color32 bg = 0;
    };
    PenOut pen{};

    // Index-native exporter LUTs.
    auto& cs = phos::color::GetColorSystem();
    const phos::color::QuantizePolicy qpol = phos::color::DefaultQuantizePolicy();
    phos::color::PaletteInstanceId src_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
        src_pal = *id;
    const phos::color::PaletteInstanceId dst_vga16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);
    const phos::color::PaletteInstanceId dst_xterm256 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    const phos::color::PaletteInstanceId dst_xterm240 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm240Safe);
    const auto remap_to_vga16 = cs.Luts().GetOrBuildRemap(cs.Palettes(), src_pal, dst_vga16, qpol);
    const auto remap_to_xterm256 = cs.Luts().GetOrBuildRemap(cs.Palettes(), src_pal, dst_xterm256, qpol);
    const auto remap_to_xterm240 = options.xterm_240_safe ? cs.Luts().GetOrBuildRemap(cs.Palettes(), src_pal, dst_xterm240, qpol) : nullptr;
    const phos::color::Palette* pal240 = options.xterm_240_safe ? cs.Palettes().Get(dst_xterm240) : nullptr;

    auto map_xterm240_derived_to_parent = [&](std::uint16_t didx) -> int {
        if (pal240 && pal240->derived && didx < pal240->derived->derived_to_parent.size())
            return (int)pal240->derived->derived_to_parent[(size_t)didx];
        return 16 + (int)didx; // fallback: preserve expected xterm index range
    };

    auto remap_src_to_vga16_idx = [&](AnsiCanvas::ColorIndex16 idx, int fallback) -> int {
        if (idx == AnsiCanvas::kUnsetIndex16)
            return fallback;
        if (remap_to_vga16 && (size_t)idx < remap_to_vga16->remap.size())
            return (int)remap_to_vga16->remap[(size_t)idx];
        // Budget-pressure fallback: exact scan via packed color round-trip.
        const std::uint32_t c32 =
            phos::color::ColorOps::IndexToColor32(cs.Palettes(), src_pal, phos::color::ColorIndex{idx});
        const phos::color::ColorIndex di =
            phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_vga16, c32, qpol);
        return di.IsUnset() ? fallback : (int)std::clamp<int>((int)di.v, 0, 15);
    };

    auto remap_src_to_xterm256_idx = [&](AnsiCanvas::ColorIndex16 idx, int fallback) -> int {
        if (idx == AnsiCanvas::kUnsetIndex16)
            return fallback;
        if (remap_to_xterm256 && (size_t)idx < remap_to_xterm256->remap.size())
            return (int)remap_to_xterm256->remap[(size_t)idx];
        // Budget-pressure fallback: exact scan via packed color round-trip.
        const std::uint32_t c32 =
            phos::color::ColorOps::IndexToColor32(cs.Palettes(), src_pal, phos::color::ColorIndex{idx});
        const phos::color::ColorIndex di =
            phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_xterm256, c32, qpol);
        return di.IsUnset() ? fallback : (int)std::clamp<int>((int)di.v, 0, 255);
    };

    auto remap_src_to_xterm240_idx = [&](AnsiCanvas::ColorIndex16 idx, int fallback) -> int {
        if (idx == AnsiCanvas::kUnsetIndex16)
            return fallback;
        if (remap_to_xterm240 && (size_t)idx < remap_to_xterm240->remap.size())
            return map_xterm240_derived_to_parent(remap_to_xterm240->remap[(size_t)idx]);
        // Budget-pressure fallback: exact scan via packed color round-trip.
        const std::uint32_t c32 =
            phos::color::ColorOps::IndexToColor32(cs.Palettes(), src_pal, phos::color::ColorIndex{idx});
        const phos::color::ColorIndex didx =
            phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_xterm240, c32, qpol);
        if (didx.IsUnset())
            return fallback;
        return map_xterm240_derived_to_parent((std::uint16_t)didx.v);
    };

    auto color32_to_xterm256 = [&](AnsiCanvas::Color32 c32, int fallback) -> int {
        const phos::color::ColorIndex idx = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_xterm256, (std::uint32_t)c32, qpol);
        return idx.IsUnset() ? fallback : (int)std::clamp<int>((int)idx.v, 0, 255);
    };

    auto color32_to_xterm240 = [&](AnsiCanvas::Color32 c32, int fallback) -> int {
        const phos::color::ColorIndex didx = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), dst_xterm240, (std::uint32_t)c32, qpol);
        if (didx.IsUnset())
            return fallback;
        return map_xterm240_derived_to_parent((std::uint16_t)didx.v);
    };

    const int default_fg_xterm =
        (options.default_fg != 0) ? color32_to_xterm256(options.default_fg, 7) : 7;
    const int default_bg_xterm =
        (options.default_bg != 0) ? color32_to_xterm256(options.default_bg, 0) : 0;
    const int default_fg_xterm240 =
        options.xterm_240_safe
            ? ((options.default_fg != 0) ? color32_to_xterm240(options.default_fg, 16)
                                         : color32_to_xterm240((AnsiCanvas::Color32)xterm256::Color32ForIndex(7), 16))
            : 7;
    const int default_bg_xterm240 =
        options.xterm_240_safe
            ? ((options.default_bg != 0) ? color32_to_xterm240(options.default_bg, 16)
                                         : color32_to_xterm240((AnsiCanvas::Color32)xterm256::Color32ForIndex(0), 16))
            : 0;

    auto reset = [&]() {
        EmitSgr(out_bytes, "0");
        pen = PenOut{};
    };

    auto ensure_sgr_for_cell = [&](const ExportCell& c)
    {
        // Resolve unset -> default behavior.
        const bool fg_unset = (c.fg_idx == AnsiCanvas::kUnsetIndex16);
        const bool bg_unset = (c.bg_idx == AnsiCanvas::kUnsetIndex16);

        // Attribute filtering based on output target.
        AnsiCanvas::Attrs want_attrs = c.attrs;
        AnsiCanvas::Attrs allowed_attrs = 0;
        if (options.attribute_mode == ExportOptions::AttributeMode::ClassicDos)
        {
            allowed_attrs = (AnsiCanvas::Attrs)(AnsiCanvas::Attr_Bold | AnsiCanvas::Attr_Blink | AnsiCanvas::Attr_Reverse);
        }
        else
        {
            allowed_attrs = (AnsiCanvas::Attrs)(AnsiCanvas::Attr_Bold | AnsiCanvas::Attr_Dim | AnsiCanvas::Attr_Italic |
                                                AnsiCanvas::Attr_Underline | AnsiCanvas::Attr_Blink | AnsiCanvas::Attr_Reverse |
                                                AnsiCanvas::Attr_Strikethrough);
        }
        want_attrs = (AnsiCanvas::Attrs)(want_attrs & allowed_attrs);

        const bool want_bold = (want_attrs & AnsiCanvas::Attr_Bold) != 0;
        const bool want_dim = (want_attrs & AnsiCanvas::Attr_Dim) != 0;
        const bool want_italic = (want_attrs & AnsiCanvas::Attr_Italic) != 0;
        const bool want_underline = (want_attrs & AnsiCanvas::Attr_Underline) != 0;
        const bool want_blink = (want_attrs & AnsiCanvas::Attr_Blink) != 0;
        const bool want_invert = (want_attrs & AnsiCanvas::Attr_Reverse) != 0;
        const bool want_strike = (want_attrs & AnsiCanvas::Attr_Strikethrough) != 0;

        // Pablo/Icy truecolor `...t` mode:
        // Optionally emit an ANSI16 baseline and overlay `...t` only when needed.
        if (options.color_mode == ExportOptions::ColorMode::TrueColorPabloT)
        {
            const AnsiCanvas::Color32 want_fg = fg_unset ? DefaultFgForExport(options) : c.fg;
            const AnsiCanvas::Color32 want_bg = bg_unset ? DefaultBgForExport(options) : c.bg;

            // First, optionally reset to defaults when colors are "unset" (no overlay for unset).
            std::string reset_params;
            if (fg_unset && options.use_default_fg_39 && (pen.has_fg || pen.fg_tc))
            {
                if (!reset_params.empty()) reset_params.push_back(';');
                reset_params += "39";
                pen.has_fg = false;
                pen.fg_tc = false;
            }
            if (bg_unset && options.use_default_bg_49 && (pen.has_bg || pen.bg_tc))
            {
                if (!reset_params.empty()) reset_params.push_back(';');
                reset_params += "49";
                pen.has_bg = false;
                pen.bg_tc = false;
            }
            if (!reset_params.empty())
                EmitSgr(out_bytes, reset_params);

            if (options.pablo_t_with_ansi16_fallback)
            {
                // ANSI16 baseline
                const int fg16 = fg_unset ? 7 : remap_src_to_vga16_idx(c.fg_idx, 7);
                const int bg16 = bg_unset ? 0 : remap_src_to_vga16_idx(c.bg_idx, 0);

                bool want_bold16 = false;
                bool want_blink16 = false;
                int fg_base = fg16;
                int bg_base = bg16;
                if (options.ansi16_bright == ExportOptions::Ansi16Bright::BoldAndIceBlink)
                {
                    if (fg_base >= 8) { want_bold16 = true; fg_base -= 8; }
                    if (options.icecolors && bg_base >= 8) { want_blink16 = true; bg_base -= 8; }
                }

                // If we need to turn attributes OFF, simplest is reset + rebuild.
                if ((pen.bold && !want_bold16) || (pen.blink && !want_blink16))
                    reset();

                std::string params;
                auto add = [&](int v) {
                    if (!params.empty()) params.push_back(';');
                    params += std::to_string(v);
                };

                // Reverse video (SGR 7/27). (Other attrs are intentionally ignored in ANSI16/Pablo-T export.)
                if (want_invert && !pen.invert) add(7);
                if (!want_invert && pen.invert) add(27);

                if (options.ansi16_bright == ExportOptions::Ansi16Bright::Sgr90_100)
                {
                    const int fg_code = (fg16 < 8) ? (30 + fg16) : (90 + (fg16 - 8));
                    const int bg_code = (bg16 < 8) ? (40 + bg16) : (100 + (bg16 - 8));

                    // If we were previously in `...t` for this channel, we MUST emit the SGR baseline
                    // to clear the truecolor override in consumers like libansilove/Pablo.
                    if (pen.fg_tc || (!pen.has_fg || pen.fg_idx != fg16)) add(fg_code);
                    if (pen.bg_tc || (!pen.has_bg || pen.bg_idx != bg16)) add(bg_code);
                }
                else
                {
                    if (want_bold16 && !pen.bold) add(1);
                    if (want_blink16 && !pen.blink) add(5);
                    if (pen.fg_tc || (!pen.has_fg || pen.fg_idx != fg16)) add(30 + fg_base);
                    if (pen.bg_tc || (!pen.has_bg || pen.bg_idx != bg16)) add(40 + bg_base);
                }

                if (!params.empty())
                    EmitSgr(out_bytes, params);

                pen.bold = want_bold16;
                pen.blink = want_blink16;
                pen.invert = want_invert;
                pen.has_fg = true;
                pen.has_bg = true;
                pen.fg_idx = fg16;
                pen.bg_idx = bg16;
                pen.fg = want_fg;
                pen.bg = want_bg;
                pen.fg_tc = false;
                pen.bg_tc = false;

                // Conditional `...t` overlay when the baseline doesn't match.
                // (Exact equality check is fine because both are packed ABGR.)
                if (!fg_unset)
                {
                    const auto base_fg = Vga16Color32ForIndex(fg16);
                    if (want_fg != base_fg)
                    {
                        std::uint8_t r = 0, g = 0, b = 0;
                        UnpackImGuiCol32(want_fg, r, g, b);
                        out_bytes.push_back(ESC);
                        out_bytes.push_back((std::uint8_t)'[');
                        const std::string s = "1;" + std::to_string((int)r) + ";" + std::to_string((int)g) + ";" + std::to_string((int)b);
                        out_bytes.insert(out_bytes.end(), s.begin(), s.end());
                        out_bytes.push_back((std::uint8_t)'t');
                        pen.fg_tc = true;
                    }
                }
                if (!bg_unset)
                {
                    const auto base_bg = Vga16Color32ForIndex(bg16);
                    if (want_bg != base_bg)
                    {
                        std::uint8_t r = 0, g = 0, b = 0;
                        UnpackImGuiCol32(want_bg, r, g, b);
                        out_bytes.push_back(ESC);
                        out_bytes.push_back((std::uint8_t)'[');
                        const std::string s = "0;" + std::to_string((int)r) + ";" + std::to_string((int)g) + ";" + std::to_string((int)b);
                        out_bytes.insert(out_bytes.end(), s.begin(), s.end());
                        out_bytes.push_back((std::uint8_t)'t');
                        pen.bg_tc = true;
                    }
                }
            }
            else
            {
                // Pure `...t` mode: attributes are emitted via SGR separately.
                if (want_invert != pen.invert)
                {
                    EmitSgr(out_bytes, want_invert ? "7" : "27");
                    pen.invert = want_invert;
                }

                // Pure `...t` mode: only emit `...t` for non-unset channels.
                if (!fg_unset && (!pen.has_fg || pen.fg != want_fg || !pen.fg_tc))
                {
                    std::uint8_t r = 0, g = 0, b = 0;
                    UnpackImGuiCol32(want_fg, r, g, b);
                    out_bytes.push_back(ESC);
                    out_bytes.push_back((std::uint8_t)'[');
                    const std::string s = "1;" + std::to_string((int)r) + ";" + std::to_string((int)g) + ";" + std::to_string((int)b);
                    out_bytes.insert(out_bytes.end(), s.begin(), s.end());
                    out_bytes.push_back((std::uint8_t)'t');
                    pen.has_fg = true;
                    pen.fg = want_fg;
                    pen.fg_tc = true;
                }
                if (!bg_unset && (!pen.has_bg || pen.bg != want_bg || !pen.bg_tc))
                {
                    std::uint8_t r = 0, g = 0, b = 0;
                    UnpackImGuiCol32(want_bg, r, g, b);
                    out_bytes.push_back(ESC);
                    out_bytes.push_back((std::uint8_t)'[');
                    const std::string s = "0;" + std::to_string((int)r) + ";" + std::to_string((int)g) + ";" + std::to_string((int)b);
                    out_bytes.insert(out_bytes.end(), s.begin(), s.end());
                    out_bytes.push_back((std::uint8_t)'t');
                    pen.has_bg = true;
                    pen.bg = want_bg;
                    pen.bg_tc = true;
                }
            }

            return;
        }

        if (options.color_mode == ExportOptions::ColorMode::Ansi16)
        {
            const int fg16 = fg_unset ? 7 : remap_src_to_vga16_idx(c.fg_idx, 7);
            const int bg16 = bg_unset ? 0 : remap_src_to_vga16_idx(c.bg_idx, 0);

            // Map into classic SGR codes.
            bool want_bold16 = false;
            bool want_blink16 = false;
            int fg_base = fg16;
            int bg_base = bg16;

            if (options.ansi16_bright == ExportOptions::Ansi16Bright::BoldAndIceBlink)
            {
                if (fg_base >= 8) { want_bold16 = true; fg_base -= 8; }
                if (options.icecolors && bg_base >= 8) { want_blink16 = true; bg_base -= 8; }
            }

            // If we need to turn attributes OFF, simplest is reset + rebuild.
            if ((pen.bold && !want_bold16) || (pen.blink && !want_blink16))
            {
                reset();
            }

            std::string params;
            auto add = [&](int v) {
                if (!params.empty()) params.push_back(';');
                params += std::to_string(v);
            };

            // Reverse video (SGR 7/27). (Other attrs are intentionally ignored in ANSI16 export.)
            if (want_invert && !pen.invert) add(7);
            if (!want_invert && pen.invert) add(27);

            if (options.ansi16_bright == ExportOptions::Ansi16Bright::Sgr90_100)
            {
                // Emit direct bright codes when needed; background uses 100-107.
                const int fg_code = (fg16 < 8) ? (30 + fg16) : (90 + (fg16 - 8));
                const int bg_code = (bg16 < 8) ? (40 + bg16) : (100 + (bg16 - 8));

                if (!pen.has_fg || pen.fg_idx != fg16) add(fg_code);
                if (!pen.has_bg || pen.bg_idx != bg16) add(bg_code);
            }
            else
            {
                if (want_bold16 && !pen.bold) add(1);
                if (want_blink16 && !pen.blink) add(5);
                if (!pen.has_fg || pen.fg_idx != fg16) add(30 + fg_base);
                if (!pen.has_bg || pen.bg_idx != bg16) add(40 + bg_base);
            }

            if (!params.empty())
                EmitSgr(out_bytes, params);

            pen.bold = want_bold16;
            pen.blink = want_blink16;
            pen.invert = want_invert;
            pen.has_fg = true;
            pen.has_bg = true;
            pen.fg_idx = fg16;
            pen.bg_idx = bg16;
            pen.fg_tc = false;
            pen.bg_tc = false;
            return;
        }

        // Modern modes: allow "default" resets for unset fg/bg.
        std::string params;
        auto add = [&](std::string_view s) {
            if (!params.empty()) params.push_back(';');
            params.append(s.begin(), s.end());
        };
        auto add_int = [&](int v) { add(std::to_string(v)); };

        // Attributes (SGR effects).
        //
        // Notes:
        // - We treat bold+dim as a coupled "intensity" group because SGR 22 resets both.
        // - Attributes are emitted only for modern color modes (xterm256 / truecolor SGR),
        //   and filtered by ExportOptions::attribute_mode above.
        {
            const bool need_reset22 = (pen.bold && !want_bold) || (pen.dim && !want_dim);
            if (need_reset22)
            {
                add("22");
                pen.bold = false;
                pen.dim = false;
            }
            if (want_bold && !pen.bold) { add("1"); pen.bold = true; }
            if (want_dim && !pen.dim)   { add("2"); pen.dim = true; }

            if (pen.italic != want_italic) { add(want_italic ? "3" : "23"); pen.italic = want_italic; }
            if (pen.underline != want_underline) { add(want_underline ? "4" : "24"); pen.underline = want_underline; }
            if (pen.blink != want_blink) { add(want_blink ? "5" : "25"); pen.blink = want_blink; }
            if (pen.invert != want_invert) { add(want_invert ? "7" : "27"); pen.invert = want_invert; }
            if (pen.strike != want_strike) { add(want_strike ? "9" : "29"); pen.strike = want_strike; }
        }

        // Foreground
        if (fg_unset && options.use_default_fg_39)
        {
            if (pen.has_fg)
            {
                add("39");
                pen.has_fg = false;
                pen.fg_tc = false;
            }
        }
        else
        {
            if (options.color_mode == ExportOptions::ColorMode::Xterm256)
            {
                const int idx =
                    fg_unset
                        ? (options.xterm_240_safe ? default_fg_xterm240 : default_fg_xterm)
                        : (options.xterm_240_safe ? remap_src_to_xterm240_idx(c.fg_idx, default_fg_xterm240)
                                                  : remap_src_to_xterm256_idx(c.fg_idx, default_fg_xterm));
                if (!pen.has_fg || pen.fg_idx != idx)
                {
                    add_int(38); add_int(5); add_int(idx);
                    pen.has_fg = true;
                    pen.fg_idx = idx;
                    pen.fg_tc = false;
                }
            }
            else if (options.color_mode == ExportOptions::ColorMode::TrueColorSgr)
            {
                const AnsiCanvas::Color32 want_fg = fg_unset ? DefaultFgForExport(options) : c.fg;
                if (!pen.has_fg || pen.fg != want_fg)
                {
                    std::uint8_t r = 0, g = 0, b = 0;
                    UnpackImGuiCol32(want_fg, r, g, b);
                    add_int(38); add_int(2); add_int(r); add_int(g); add_int(b);
                    pen.has_fg = true;
                    pen.fg = want_fg;
                    pen.fg_tc = false;
                }
            }
        }

        // Background
        if (bg_unset && options.use_default_bg_49)
        {
            if (pen.has_bg)
            {
                add("49");
                pen.has_bg = false;
                pen.bg_tc = false;
            }
        }
        else
        {
            if (options.color_mode == ExportOptions::ColorMode::Xterm256)
            {
                const int idx =
                    bg_unset
                        ? (options.xterm_240_safe ? default_bg_xterm240 : default_bg_xterm)
                        : (options.xterm_240_safe ? remap_src_to_xterm240_idx(c.bg_idx, default_bg_xterm240)
                                                  : remap_src_to_xterm256_idx(c.bg_idx, default_bg_xterm));
                if (!pen.has_bg || pen.bg_idx != idx)
                {
                    add_int(48); add_int(5); add_int(idx);
                    pen.has_bg = true;
                    pen.bg_idx = idx;
                    pen.bg_tc = false;
                }
            }
            else if (options.color_mode == ExportOptions::ColorMode::TrueColorSgr)
            {
                const AnsiCanvas::Color32 want_bg = bg_unset ? DefaultBgForExport(options) : c.bg;
                if (!pen.has_bg || pen.bg != want_bg)
                {
                    std::uint8_t r = 0, g = 0, b = 0;
                    UnpackImGuiCol32(want_bg, r, g, b);
                    add_int(48); add_int(2); add_int(r); add_int(g); add_int(b);
                    pen.has_bg = true;
                    pen.bg = want_bg;
                    pen.bg_tc = false;
                }
            }
        }

        if (!params.empty())
            EmitSgr(out_bytes, params);

    };

    auto emit_newline = [&]() {
        if (options.newline == ExportOptions::Newline::CRLF)
        {
            out_bytes.push_back(CR);
            out_bytes.push_back(LF);
        }
        else
        {
            out_bytes.push_back(LF);
        }
    };

    auto bg_defaultish_for_cell = [&](const ExportCell& c) -> bool {
        const bool bg_unset = (c.bg_idx == AnsiCanvas::kUnsetIndex16);
        if (bg_unset)
            return true;

        if (options.color_mode == ExportOptions::ColorMode::TrueColorSgr ||
            options.color_mode == ExportOptions::ColorMode::TrueColorPabloT)
        {
            return c.bg == DefaultBgForExport(options);
        }

        if (options.color_mode == ExportOptions::ColorMode::Ansi16)
        {
            const int bg16 =
                remap_src_to_vga16_idx(c.bg_idx, 0);
            return bg16 == 0;
        }

        if (options.color_mode == ExportOptions::ColorMode::Xterm256)
        {
            const int def = options.xterm_240_safe ? default_bg_xterm240 : default_bg_xterm;
            const int bgx = options.xterm_240_safe ? remap_src_to_xterm240_idx(c.bg_idx, def)
                                                   : remap_src_to_xterm256_idx(c.bg_idx, def);
            return bgx == def;
        }

        return true;
    };

    // Export row-major.
    for (int y = 0; y < rows; ++y)
    {
        int x_end = cols - 1;
        if (!options.preserve_line_length)
        {
            // Trim trailing "safe blanks": blank-ish char and background effectively default.
            x_end = -1;
            for (int x = cols - 1; x >= 0; --x)
            {
                ExportCell c;
                if (!SampleCell(canvas, options, y, x, c))
                    continue;

                const bool bg_defaultish = bg_defaultish_for_cell(c);
                const bool interesting = !IsBlankish(c.glyph) || !bg_defaultish || (c.attrs != 0);
                if (interesting)
                {
                    x_end = x;
                    break;
                }
            }
        }

        int x = 0;
        while (x <= x_end)
        {
            ExportCell c;
            (void)SampleCell(canvas, options, y, x, c);

            // Optional cursor-forward compression for safe space runs.
            if (options.compress && options.use_cursor_forward)
            {
                const bool bg_defaultish = bg_defaultish_for_cell(c);
                if (phos::glyph::IsBlank(c.glyph) && bg_defaultish && c.attrs == 0)
                {
                    int run = 1;
                    while (x + run <= x_end)
                    {
                        ExportCell n;
                        (void)SampleCell(canvas, options, y, x + run, n);
                        const bool n_bg_defaultish = bg_defaultish_for_cell(n);
                        if (!phos::glyph::IsBlank(n.glyph) || !n_bg_defaultish || n.attrs != 0)
                            break;
                        run++;
                    }

                    // Only worthwhile if CSI n C is shorter than run spaces.
                    const int esc_len = 3 + Digits10(run); // ESC[ + digits + 'C'
                    if (esc_len < run)
                    {
                        // Ensure background/fg is reset to defaults for semantic equivalence.
                        // For modern modes, prefer 39/49; otherwise use reset.
                        if (options.color_mode == ExportOptions::ColorMode::Ansi16)
                        {
                            // We can only "skip painting" if pen bg is default (black).
                            // Reset makes it so.
                            if (pen.has_bg && pen.bg_idx != 0)
                                reset();
                        }
                        else
                        {
                            std::string p;
                            if (pen.has_fg && options.use_default_fg_39) { if (!p.empty()) p.push_back(';'); p += "39"; pen.has_fg = false; }
                            if (pen.has_bg && options.use_default_bg_49) { if (!p.empty()) p.push_back(';'); p += "49"; pen.has_bg = false; }
                            if (!p.empty()) EmitSgr(out_bytes, p);
                        }

                        EmitCsi(out_bytes, std::to_string(run), 'C');
                        x += run;
                        continue;
                    }
                }
            }

            ensure_sgr_for_cell(c);

            // Emit glyph bytes.
            if (options.text_encoding == ExportOptions::TextEncoding::Cp437)
            {
                std::uint8_t b = (std::uint8_t)'?';
                const phos::glyph::Kind k = phos::glyph::GetKind(c.glyph);
                if (k == phos::glyph::Kind::BitmapIndex)
                {
                    const std::uint16_t idx = phos::glyph::BitmapIndexValue(c.glyph);
                    b = (idx <= 0xFFu) ? (std::uint8_t)idx : (std::uint8_t)'?';
                }
                else if (k == phos::glyph::Kind::EmbeddedIndex)
                {
                    const std::uint16_t idx = phos::glyph::EmbeddedIndexValue(c.glyph);
                    b = (idx <= 0xFFu) ? (std::uint8_t)idx : (std::uint8_t)'?';
                }
                else
                {
                    // UnicodeScalar (or defensive fallback): map using the selected byte encoding.
                    b = UnicodeToByteOrFallback(options.byte_encoding, c.cp, (std::uint8_t)'?');
                }
                out_bytes.push_back(b);
            }
            else
            {
                // Filter ASCII control chars (except we never expect them in cells).
                char32_t cp = c.cp;
                if (cp < 0x20)
                    cp = U' ';
                Utf8Append(cp, out_bytes);
            }

            x++;
        }

        // End of row.
        emit_newline();
    }

    if (options.final_reset)
        reset();

    // Optional SAUCE append.
    if (options.write_sauce)
    {
        sauce::Record r;
        // Prefer existing canvas SAUCE metadata, but ensure present and sane.
        const auto& meta = canvas.GetSauceMeta();
        r.present = true;
        r.title = meta.title;
        r.author = meta.author;
        r.group = meta.group;
        r.date = meta.date;
        r.file_size = (std::uint32_t)out_bytes.size();
        r.data_type = (std::uint8_t)sauce::DataType::Character;
        r.file_type = 1; // ANSi
        r.tinfo1 = (std::uint16_t)std::clamp(cols, 0, 65535);
        r.tinfo2 = (std::uint16_t)std::clamp(rows, 0, 65535);
        r.tinfo3 = meta.tinfo3;
        r.tinfo4 = meta.tinfo4;
        r.tflags = meta.tflags;
        r.tinfos = meta.tinfos;
        r.comments = meta.comments;

        std::string serr;
        const auto with_sauce = sauce::AppendToBytes(out_bytes, r, options.sauce_write_options, serr);
        if (with_sauce.empty() && !serr.empty())
        {
            err = serr;
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
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    if (!out)
    {
        err = "Failed to write file contents.";
        return false;
    }
    return true;
}

const std::vector<Preset>& Presets()
{
    static const std::vector<Preset> presets = []() {
        std::vector<Preset> v;
        v.reserve(16);

        {
            Preset p;
            p.id = PresetId::SceneClassic;
            p.name = "Scene Classic (CP437 + ANSI16)";
            p.description = "Classic ANSI art interchange: CP437 bytes, 16-color SGR, CRLF, optional SAUCE.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Cp437;
            p.export_.color_mode = ExportOptions::ColorMode::Ansi16;
            p.export_.attribute_mode = ExportOptions::AttributeMode::ClassicDos;
            p.export_.ansi16_bright = ExportOptions::Ansi16Bright::BoldAndIceBlink;
            p.export_.icecolors = true;
            p.export_.newline = ExportOptions::Newline::CRLF;
            p.export_.preserve_line_length = true;
            p.export_.write_sauce = true;
            p.export_.sauce_write_options.include_eof_byte = true;
            p.export_.sauce_write_options.include_comments = true;
            p.export_.sauce_write_options.encode_cp437 = true;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::ModernUtf8_240Safe;
            p.name = "Modern Terminal (UTF-8 + 240-color safe)";
            p.description = "UTF-8 text with xterm indexed colors, remapping low-16 palette to stable 16..255; LF; no SAUCE by default.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8;
            p.export_.color_mode = ExportOptions::ColorMode::Xterm256;
            p.export_.attribute_mode = ExportOptions::AttributeMode::Modern;
            p.export_.xterm_240_safe = true;
            p.export_.newline = ExportOptions::Newline::LF;
            p.export_.preserve_line_length = false;
            p.export_.write_sauce = false;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::ModernUtf8_256;
            p.name = "Modern Terminal (UTF-8 + 256-color)";
            p.description = "UTF-8 text with xterm indexed colors 0..255; LF; no SAUCE by default.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8;
            p.export_.color_mode = ExportOptions::ColorMode::Xterm256;
            p.export_.attribute_mode = ExportOptions::AttributeMode::Modern;
            p.export_.xterm_240_safe = false;
            p.export_.newline = ExportOptions::Newline::LF;
            p.export_.preserve_line_length = false;
            p.export_.write_sauce = false;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::TruecolorSgr_Utf8;
            p.name = "Truecolor (UTF-8 + 38;2/48;2)";
            p.description = "UTF-8 text with standard-ish truecolor SGR; LF; no SAUCE by default.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8;
            p.export_.color_mode = ExportOptions::ColorMode::TrueColorSgr;
            p.export_.attribute_mode = ExportOptions::AttributeMode::Modern;
            p.export_.newline = ExportOptions::Newline::LF;
            p.export_.preserve_line_length = false;
            p.export_.write_sauce = false;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::TruecolorPabloT_Cp437;
            p.name = "Pablo/Icy Truecolor (CP437 + ANSI16 fallback + ...t)";
            p.description = "Scene-friendly: CP437 + ANSI16 baseline (bold/iCE), with Pablo/Icy `...t` RGB overlay when needed; CRLF; SAUCE on.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Cp437;
            p.export_.color_mode = ExportOptions::ColorMode::TrueColorPabloT;
            p.export_.attribute_mode = ExportOptions::AttributeMode::ClassicDos;
            p.export_.pablo_t_with_ansi16_fallback = true;
            p.export_.ansi16_bright = ExportOptions::Ansi16Bright::BoldAndIceBlink;
            p.export_.icecolors = true;
            p.export_.newline = ExportOptions::Newline::CRLF;
            p.export_.preserve_line_length = true;
            p.export_.write_sauce = true;
            p.export_.sauce_write_options.include_eof_byte = true;
            p.export_.sauce_write_options.include_comments = true;
            p.export_.sauce_write_options.encode_cp437 = true;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::Durdraw_Utf8_256;
            p.name = "Durdraw (UTF-8 + 256-color)";
            p.description = "Durdraw-style terminal output: UTF-8 + 38;5/48;5, LF, no SAUCE.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8;
            p.export_.color_mode = ExportOptions::ColorMode::Xterm256;
            p.export_.attribute_mode = ExportOptions::AttributeMode::Modern;
            p.export_.newline = ExportOptions::Newline::LF;
            p.export_.preserve_line_length = true; // Durdraw tends to be fixed-grid-ish per export.
            p.export_.write_sauce = false;
            p.export_.compress = false; // Durdraw emits attributes per cell; we won't mimic that here, but disable our compression.
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::Moebius_Classic;
            p.name = "Moebius (Classic)";
            p.description = "Moebius classic: CP437 + ANSI16 + CRLF + SAUCE (+^Z).";
            p.export_.text_encoding = ExportOptions::TextEncoding::Cp437;
            p.export_.color_mode = ExportOptions::ColorMode::Ansi16;
            p.export_.attribute_mode = ExportOptions::AttributeMode::ClassicDos;
            p.export_.ansi16_bright = ExportOptions::Ansi16Bright::BoldAndIceBlink;
            p.export_.newline = ExportOptions::Newline::CRLF;
            p.export_.write_sauce = true;
            p.export_.sauce_write_options.include_eof_byte = true;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::PabloDraw_Classic;
            p.name = "PabloDraw (Classic)";
            p.description = "PabloDraw-friendly: CP437 + ANSI16; allow cursor-forward compression on safe spaces; CRLF; optional SAUCE.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Cp437;
            p.export_.color_mode = ExportOptions::ColorMode::Ansi16;
            p.export_.attribute_mode = ExportOptions::AttributeMode::ClassicDos;
            p.export_.ansi16_bright = ExportOptions::Ansi16Bright::BoldAndIceBlink;
            p.export_.newline = ExportOptions::Newline::CRLF;
            p.export_.preserve_line_length = false;
            p.export_.compress = true;
            p.export_.use_cursor_forward = true;
            p.export_.write_sauce = true;
            v.push_back(p);
        }

        {
            Preset p;
            p.id = PresetId::IcyDraw_Modern;
            p.name = "Icy Draw (Modern)";
            p.description = "Icy-style modern output: UTF-8 (BOM) + xterm256 or truecolor; LF; SAUCE optional.";
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8Bom;
            p.export_.color_mode = ExportOptions::ColorMode::Xterm256;
            p.export_.attribute_mode = ExportOptions::AttributeMode::Modern;
            p.export_.newline = ExportOptions::Newline::LF;
            p.export_.preserve_line_length = false;
            p.export_.compress = true;
            p.export_.use_cursor_forward = true;
            p.export_.write_sauce = false;
            v.push_back(p);
        }

        return v;
    }();
    return presets;
}

const Preset* FindPreset(PresetId id)
{
    const auto& p = Presets();
    for (const auto& it : p)
        if (it.id == id)
            return &it;
    return nullptr;
}
} // namespace ansi
} // namespace formats


