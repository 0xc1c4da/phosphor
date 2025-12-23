#include "app/clipboard_utils.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

#include "ansl/ansl_native.h"
#include "core/canvas.h"
#include "core/color_system.h"
#include "io/formats/ansi.h"

namespace app
{
namespace
{
static bool ContainsEsc(std::string_view s)
{
    for (unsigned char ch : s)
        if (ch == 0x1B)
            return true;
    return false;
}

static bool IsTrimBlank(char32_t cp)
{
    return cp == U' ' || cp == U'\0';
}

static std::string SelectionToUtf8Text(const AnsiCanvas& canvas)
{
    if (!canvas.HasSelection())
        return {};

    const AnsiCanvas::Rect r = canvas.GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return {};

    std::string out;
    out.reserve((size_t)r.w * (size_t)r.h);

    for (int j = 0; j < r.h; ++j)
    {
        // Gather row as cps and trim trailing blanks (for nicer plain text clipboard).
        std::vector<char32_t> line;
        line.resize((size_t)r.w, U' ');

        for (int i = 0; i < r.w; ++i)
        {
            const int x = r.x + i;
            const int y = r.y + j;
            char32_t cp = U' ';
            AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::Attrs attrs = 0;
            (void)canvas.GetCompositeCellPublicIndices(y, x, cp, fg, bg, attrs);
            line[(size_t)i] = cp;
        }

        while (!line.empty() && IsTrimBlank(line.back()))
            line.pop_back();

        for (char32_t cp : line)
            out += ansl::utf8::encode(cp);

        if (j + 1 < r.h)
            out.push_back('\n');
    }

    return out;
}

// Decode clipboard UTF-8 bytes into lines of codepoints.
// - Handles CRLF/CR
// - Expands TAB to 8-column tab stops using spaces.
static void DecodePlainTextToGrid(std::string_view utf8,
                                  std::vector<std::vector<char32_t>>& out_lines,
                                  int& out_w,
                                  int& out_h)
{
    out_lines.clear();
    out_w = 0;
    out_h = 0;

    std::vector<char32_t> cps;
    ansl::utf8::decode_to_codepoints(utf8.data(), utf8.size(), cps);

    std::vector<char32_t> cur;
    cur.reserve(128);
    int col = 0;
    bool last_was_cr = false;

    auto flush_line = [&]() {
        out_w = std::max(out_w, (int)cur.size());
        out_lines.push_back(cur);
        cur.clear();
        col = 0;
    };

    for (char32_t cp : cps)
    {
        if (cp == U'\n')
        {
            flush_line();
            last_was_cr = false;
            continue;
        }
        if (cp == U'\r')
        {
            flush_line();
            last_was_cr = true;
            continue;
        }
        if (last_was_cr)
        {
            // Convert CRLF to a single newline (we already flushed on CR).
            last_was_cr = false;
        }

        if (cp == U'\t')
        {
            const int tab_w = 8;
            const int next = ((col / tab_w) + 1) * tab_w;
            while (col < next)
            {
                cur.push_back(U' ');
                col++;
            }
            continue;
        }

        // Drop ASCII control chars for plain text paste.
        if (cp < 0x20u)
            continue;

        cur.push_back(cp);
        col++;
    }

    // Always produce at least one line.
    flush_line();
    out_h = (int)out_lines.size();
    out_w = std::max(out_w, 1);
    out_h = std::max(out_h, 1);
}
} // namespace

bool CopySelectionToSystemClipboardText(const AnsiCanvas& canvas)
{
    const std::string text = SelectionToUtf8Text(canvas);
    if (text.empty())
        return false;

    ImGui::SetClipboardText(text.c_str());
    return true;
}

bool PasteSystemClipboardText(AnsiCanvas& canvas, int x, int y)
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip)
        return false;

    std::string_view sv(clip);

    // If we have a selection, replace it and paste at its top-left.
    if (canvas.HasSelection())
    {
        const AnsiCanvas::Rect r = canvas.GetSelectionRect();
        x = r.x;
        y = r.y;
    }

    const int layer = canvas.GetActiveLayerIndex();
    if (layer < 0)
        return false;

    canvas.PushUndoSnapshot();

    if (canvas.HasSelection())
    {
        (void)canvas.DeleteSelection(layer);
        canvas.ClearSelection();
    }

    int pasted_w = 1;
    int pasted_h = 1;

    if (ContainsEsc(sv))
    {
        // ANSI paste: preserve colors/attrs.
        std::vector<std::uint8_t> bytes;
        bytes.assign(sv.begin(), sv.end());

        formats::ansi::ImportOptions opt;
        opt.columns = 0;               // auto
        opt.icecolors = true;
        opt.default_bg_unset = true;   // avoid painting black when stream relies on "default bg"
        opt.cp437 = true;              // auto-switch to UTF-8 when appropriate

        AnsiCanvas imported;
        std::string err;
        if (!formats::ansi::ImportBytesToCanvas(bytes, imported, err, opt))
            return false;

        // Compute tight bounds based on non-space glyphs (so a single-line paste doesn't become 80 cols).
        int max_row = -1;
        int max_col = -1;
        const int rows = imported.GetRows();
        const int cols = imported.GetColumns();
        for (int rr = 0; rr < rows; ++rr)
        {
            for (int cc = 0; cc < cols; ++cc)
            {
                const char32_t cp = imported.GetLayerCell(0, rr, cc);
                if (cp != U' ' && cp != U'\0')
                {
                    max_row = std::max(max_row, rr);
                    max_col = std::max(max_col, cc);
                }
            }
        }
        pasted_w = (max_col >= 0) ? (max_col + 1) : 1;
        pasted_h = (max_row >= 0) ? (max_row + 1) : 1;

        // Remap palette indices from the imported canvas palette to the destination canvas palette.
        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId pal_src = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        phos::color::PaletteInstanceId pal_dst = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        if (auto id = cs.Palettes().Resolve(imported.GetPaletteRef()))
            pal_src = *id;
        if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
            pal_dst = *id;
        const phos::color::QuantizePolicy qpol = phos::color::DefaultQuantizePolicy();
        const auto remap = cs.Luts().GetOrBuildRemap(cs.Palettes(), pal_src, pal_dst, qpol);

        auto remap_idx = [&](AnsiCanvas::ColorIndex16 src) -> AnsiCanvas::ColorIndex16 {
            if (src == AnsiCanvas::kUnsetIndex16)
                return AnsiCanvas::kUnsetIndex16;
            if (remap && (size_t)src < remap->remap.size())
                return (AnsiCanvas::ColorIndex16)remap->remap[(size_t)src];
            // Fallback: RGB round-trip through quantizer.
            const std::uint32_t c32 = phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal_src, phos::color::ColorIndex{src});
            const phos::color::ColorIndex di = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal_dst, c32, qpol);
            return di.IsUnset() ? AnsiCanvas::kUnsetIndex16 : (AnsiCanvas::ColorIndex16)di.v;
        };

        for (int rr = 0; rr < pasted_h; ++rr)
        {
            for (int cc = 0; cc < pasted_w; ++cc)
            {
                const char32_t cp = imported.GetLayerCell(0, rr, cc);
                AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::Attrs attrs = 0;
                (void)imported.GetLayerCellIndices(0, rr, cc, fg, bg);
                (void)imported.GetLayerCellAttrs(0, rr, cc, attrs);

                const AnsiCanvas::ColorIndex16 out_fg = remap_idx(fg);
                const AnsiCanvas::ColorIndex16 out_bg = remap_idx(bg);
                (void)canvas.SetLayerCellIndices(layer, y + rr, x + cc, cp, out_fg, out_bg, attrs);
            }
        }
    }
    else
    {
        // Plain text paste: glyph-only (leave existing colours intact).
        std::vector<std::vector<char32_t>> lines;
        DecodePlainTextToGrid(sv, lines, pasted_w, pasted_h);

        for (int rr = 0; rr < pasted_h; ++rr)
        {
            const auto& line = lines[(size_t)rr];
            for (int cc = 0; cc < pasted_w; ++cc)
            {
                const char32_t cp = (cc < (int)line.size()) ? line[(size_t)cc] : U' ';
                (void)canvas.SetLayerCell(layer, y + rr, x + cc, cp);
            }
        }
    }

    // Select pasted region.
    canvas.SetSelectionCorners(x, y, x + pasted_w - 1, y + pasted_h - 1);
    return true;
}
} // namespace app


