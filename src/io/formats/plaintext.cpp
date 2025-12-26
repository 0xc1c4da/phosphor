#include "io/formats/plaintext.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace formats
{
namespace plaintext
{
const std::vector<std::string_view>& ImportExtensions()
{
    // Lowercase, no leading dots.
    // We treat these as "plaintext-intent" extensions.
    static const std::vector<std::string_view> exts = {"txt", "asc"};
    return exts;
}

const std::vector<std::string_view>& ExportExtensions()
{
    // Lowercase, no leading dots.
    static const std::vector<std::string_view> exts = {"txt", "asc"};
    return exts;
}

namespace
{
static constexpr std::uint8_t LF = '\n';
static constexpr std::uint8_t CR = '\r';

static bool IsBlankish(char32_t cp)
{
    return cp == U' ' || cp == U'\0' || cp == (char32_t)0xFF;
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

// Utility: decode UTF-8 bytes into Unicode codepoints (best-effort).
// - malformed sequences are skipped
// - BOM (EF BB BF) is stripped if present
static void DecodeUtf8BestEffort(const std::vector<std::uint8_t>& bytes, std::vector<char32_t>& out_codepoints)
{
    out_codepoints.clear();

    size_t i = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        i = 3;

    const size_t len = bytes.size();
    while (i < len)
    {
        const std::uint8_t c = bytes[i];

        char32_t cp = 0;
        size_t remaining = 0;
        if ((c & 0x80) == 0)
        {
            cp = c;
            remaining = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            remaining = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            remaining = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
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
            const std::uint8_t cc = bytes[i + 1 + j];
            if ((cc & 0xC0) != 0x80)
            {
                malformed = true;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (malformed)
        {
            ++i;
            continue;
        }

        i += 1 + remaining;
        out_codepoints.push_back(cp);
    }
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
    std::vector<std::uint8_t> out;
    out.resize((size_t)sz);
    if (sz > 0)
        in.read(reinterpret_cast<char*>(out.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return {};
    }
    return out;
}

static bool SampleCell(const AnsiCanvas& canvas, const ExportOptions& opt, int row, int col, char32_t& out_cp)
{
    out_cp = U' ';
    if (opt.source == ExportOptions::Source::Composite)
    {
        char32_t cp = U' ';
        AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;
        if (!canvas.GetCompositeCellPublicIndices(row, col, cp, fg, bg))
            return false;
        out_cp = cp;
        return true;
    }

    const int layer = canvas.GetActiveLayerIndex();
    out_cp = canvas.GetLayerCell(layer, row, col);
    return true;
}
} // namespace

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options)
{
    err.clear();

    const int columns = std::max(1, options.columns);

    std::vector<char32_t> cps;
    cps.reserve(bytes.size());

    if (options.text_encoding == ImportOptions::TextEncoding::Ascii)
    {
        for (std::uint8_t b : bytes)
            cps.push_back((b <= 0x7F) ? (char32_t)b : U'?');
    }
    else
    {
        DecodeUtf8BestEffort(bytes, cps);
    }

    auto ensure_rows = [&](int rows, std::vector<char32_t>& cells) {
        if (rows <= 0) rows = 1;
        const size_t want = (size_t)rows * (size_t)columns;
        if (cells.size() < want)
            cells.resize(want, U' ');
    };

    std::vector<char32_t> cells;
    cells.reserve((size_t)columns * 25);
    ensure_rows(1, cells);

    int row = 0;
    int col = 0;
    int rowMax = 0;
    bool last_was_cr = false;

    for (char32_t cp : cps)
    {
        // Newlines.
        if (cp == U'\r')
        {
            last_was_cr = true;
            row++;
            col = 0;
            ensure_rows(row + 1, cells);
            rowMax = std::max(rowMax, row);
            continue;
        }
        if (cp == U'\n')
        {
            if (options.normalize_crlf && last_was_cr)
            {
                last_was_cr = false;
                continue;
            }
            last_was_cr = false;
            row++;
            col = 0;
            ensure_rows(row + 1, cells);
            rowMax = std::max(rowMax, row);
            continue;
        }
        last_was_cr = false;

        // Control filtering.
        if (cp == U'\t' && options.tab_to_space)
            cp = U' ';
        if (options.filter_control_chars && cp < 0x20)
            continue;

        // Write.
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (col >= columns)
        {
            row++;
            col = 0;
            ensure_rows(row + 1, cells);
        }

        const size_t idx = (size_t)row * (size_t)columns + (size_t)col;
        if (idx >= cells.size())
            ensure_rows(row + 1, cells);
        cells[idx] = cp;
        rowMax = std::max(rowMax, row);

        col++;
        if (col >= columns)
        {
            row++;
            col = 0;
            ensure_rows(row + 1, cells);
        }
    }

    const int out_rows = std::max(1, rowMax + 1);

    std::vector<AnsiCanvas::GlyphId> glyphs;
    glyphs.reserve(cells.size());
    for (char32_t cp : cells)
        glyphs.push_back(phos::glyph::MakeUnicodeScalar(cp));

    AnsiCanvas::ProjectState st;
    st.version = 14;
    st.undo_limit = 0; // unlimited by default
    st.current.columns = columns;
    st.current.rows = out_rows;
    st.current.active_layer = 0;
    st.current.caret_row = 0;
    st.current.caret_col = 0;
    st.current.layers.clear();
    st.current.layers.resize(1);
    st.current.layers[0].name = "Base";
    st.current.layers[0].visible = true;
    st.current.layers[0].cells = std::move(glyphs);

    // Phase-B/index-native defaults: use builtin xterm256 palette and leave fg/bg unset.
    st.palette_ref.is_builtin = true;
    st.palette_ref.builtin = phos::color::BuiltinPalette::Xterm256;
    st.ui_palette_ref = st.palette_ref;
    st.current.palette_ref = st.palette_ref;
    st.current.ui_palette_ref = st.ui_palette_ref;

    st.current.layers[0].fg.assign((size_t)out_rows * (size_t)columns, AnsiCanvas::kUnsetIndex16);
    st.current.layers[0].bg.assign((size_t)out_rows * (size_t)columns, AnsiCanvas::kUnsetIndex16);

    AnsiCanvas canvas(columns);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply imported plaintext state." : apply_err;
        return false;
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

    const int cols = std::max(1, canvas.GetColumns());
    const int rows = std::max(1, canvas.GetRows());

    if (options.text_encoding == ExportOptions::TextEncoding::Utf8Bom)
    {
        out_bytes.push_back(0xEF);
        out_bytes.push_back(0xBB);
        out_bytes.push_back(0xBF);
    }

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

    for (int y = 0; y < rows; ++y)
    {
        int x_end = cols - 1;
        if (!options.preserve_line_length)
        {
            x_end = -1;
            for (int x = cols - 1; x >= 0; --x)
            {
                char32_t cp = U' ';
                if (!SampleCell(canvas, options, y, x, cp))
                    continue;
                if (!IsBlankish(cp))
                {
                    x_end = x;
                    break;
                }
            }
        }

        for (int x = 0; x <= x_end; ++x)
        {
            char32_t cp = U' ';
            (void)SampleCell(canvas, options, y, x, cp);

            // Plaintext policy: avoid raw ASCII controls in output.
            if (cp < 0x20)
                cp = U' ';

            if (options.text_encoding == ExportOptions::TextEncoding::Ascii)
            {
                const std::uint8_t b = (cp <= 0x7F) ? (std::uint8_t)cp : (std::uint8_t)'?';
                out_bytes.push_back(b);
            }
            else
            {
                Utf8Append(cp, out_bytes);
            }
        }

        if (options.final_newline || y != (rows - 1))
            emit_newline();
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
        v.reserve(8);

        {
            Preset p;
            p.id = PresetId::PlainUtf8;
            p.name = "Plaintext (UTF-8)";
            p.description = "UTF-8 text only (no ANSI escape sequences).";
            p.import.text_encoding = ImportOptions::TextEncoding::Utf8;
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8;
            p.export_.newline = ExportOptions::Newline::LF;
            v.push_back(p);
        }
        {
            Preset p;
            p.id = PresetId::PlainUtf8Bom;
            p.name = "Plaintext (UTF-8 with BOM)";
            p.description = "UTF-8 text with BOM (helps some tools detect Unicode).";
            p.import.text_encoding = ImportOptions::TextEncoding::Utf8;
            p.export_.text_encoding = ExportOptions::TextEncoding::Utf8Bom;
            p.export_.newline = ExportOptions::Newline::LF;
            v.push_back(p);
        }
        {
            Preset p;
            p.id = PresetId::PlainAscii;
            p.name = "Plaintext (ASCII)";
            p.description = "7-bit ASCII only; non-ASCII characters are replaced with '?'.";
            p.import.text_encoding = ImportOptions::TextEncoding::Ascii;
            p.export_.text_encoding = ExportOptions::TextEncoding::Ascii;
            p.export_.newline = ExportOptions::Newline::LF;
            v.push_back(p);
        }

        return v;
    }();

    return presets;
}

const Preset* FindPreset(PresetId id)
{
    const auto& p = Presets();
    auto it = std::find_if(p.begin(), p.end(), [&](const Preset& v) { return v.id == id; });
    return (it == p.end()) ? nullptr : &(*it);
}
} // namespace plaintext
} // namespace formats


