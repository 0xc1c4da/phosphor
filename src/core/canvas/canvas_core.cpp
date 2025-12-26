#include "core/canvas/canvas_internal.h"

#include "core/key_bindings.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ---- inlined from canvas_core.inc ----
AnsiCanvas::AnsiCanvas(int columns)
    : m_columns(columns > 0 ? columns : 80)
{
    // New canvases should start with consistent SAUCE defaults (even before the user opens the editor).
    // Rows are always >= 1.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);

    // Default palette identity (core).
    m_palette_ref.is_builtin = true;
    m_palette_ref.builtin = phos::color::BuiltinPalette::Xterm256;

    // Default UI palette selection follows the core palette.
    m_ui_palette_ref = m_palette_ref;
}

fonts::FontId AnsiCanvas::GetFontId() const
{
    return fonts::FromSauceName(m_sauce.tinfos);
}

bool AnsiCanvas::SetFontId(fonts::FontId id)
{
    const std::string_view sname = fonts::ToSauceName(id);
    if (sname.empty())
        return false;

    if (m_sauce.tinfos == sname)
        return true;

    // Persist via SAUCE.
    m_sauce.present = true;
    m_sauce.tinfos.assign(sname.begin(), sname.end());
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);

    // Font changes affect rendering but are not part of undo/redo.
    TouchContent();
    return true;
}

void AnsiCanvas::SetZoom(float zoom)
{
    // Clamp to a sensible range so we don't generate zero-sized cells or huge buffers.
    const float min_zoom = 0.10f;
    const float max_zoom = 12.0f;
    if (zoom < min_zoom) zoom = min_zoom;
    if (zoom > max_zoom) zoom = max_zoom;
    m_zoom = zoom;
}

float AnsiCanvas::SnappedScaleForZoom(float zoom, float base_cell_w_px) const
{
    // Keep this in sync with the renderer's snapping assumptions.
    // Note: snapping depends both on user preference and on whether the current canvas font is bitmap-based.
    if (!(base_cell_w_px > 0.0f))
        base_cell_w_px = 8.0f;

    const auto snap_integer = [&](float z) -> float
    {
        float n = std::floor(z + 0.5f);
        if (n < 1.0f) n = 1.0f;
        return n;
    };
    const auto snap_pixel_aligned = [&](float z) -> float
    {
        float snapped_cell_w = std::floor(base_cell_w_px * z + 0.5f);
        if (snapped_cell_w < 1.0f)
            snapped_cell_w = 1.0f;
        return (base_cell_w_px > 0.0f) ? (snapped_cell_w / base_cell_w_px) : 1.0f;
    };

    // Detect bitmap font path (embedded fonts are always bitmap).
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const bool bitmap_font =
        embedded_font ||
        (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Migration safety: older session files used 0=Auto; treat that as PixelAligned.
    ZoomSnapMode mode = m_zoom_snap_mode;
    if ((int)mode == 0)
        mode = ZoomSnapMode::PixelAligned;

    switch (mode)
    {
        case ZoomSnapMode::IntegerScale:
            return snap_integer(zoom);
        case ZoomSnapMode::PixelAligned:
            return snap_pixel_aligned(zoom);
        default:
            // Defensive: if enum expands in future, prefer pixel-aligned.
            (void)bitmap_font;
            return snap_pixel_aligned(zoom);
    }
}

void AnsiCanvas::RequestScrollPixels(float scroll_x, float scroll_y)
{
    m_scroll_request_valid = true;
    m_scroll_request_x = scroll_x;
    m_scroll_request_y = scroll_y;
}

bool AnsiCanvas::GetCompositeCellPublic(int row, int col, char32_t& out_cp, Color32& out_fg, Color32& out_bg) const
{
    out_cp = U' ';
    out_fg = 0;
    out_bg = 0;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_cp = c.cp;
    out_fg = IndexToColor32(c.fg);
    out_bg = IndexToColor32(c.bg);
    return true;
}

bool AnsiCanvas::GetCompositeCellPublic(int row, int col, char32_t& out_cp, Color32& out_fg, Color32& out_bg, Attrs& out_attrs) const
{
    out_cp = U' ';
    out_fg = 0;
    out_bg = 0;
    out_attrs = 0;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_cp = c.cp;
    out_fg = IndexToColor32(c.fg);
    out_bg = IndexToColor32(c.bg);
    out_attrs = c.attrs;
    return true;
}

bool AnsiCanvas::GetCompositeCellPublicIndices(int row, int col, char32_t& out_cp, ColorIndex16& out_fg, ColorIndex16& out_bg) const
{
    out_cp = U' ';
    out_fg = kUnsetIndex16;
    out_bg = kUnsetIndex16;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_cp = c.cp;
    out_fg = c.fg;
    out_bg = c.bg;
    return true;
}

bool AnsiCanvas::GetCompositeCellPublicIndices(int row, int col, char32_t& out_cp, ColorIndex16& out_fg, ColorIndex16& out_bg, Attrs& out_attrs) const
{
    out_cp = U' ';
    out_fg = kUnsetIndex16;
    out_bg = kUnsetIndex16;
    out_attrs = 0;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_cp = c.cp;
    out_fg = c.fg;
    out_bg = c.bg;
    out_attrs = c.attrs;
    return true;
}

bool AnsiCanvas::GetCompositeCellPublicGlyphIndices(int row, int col, GlyphId& out_glyph, ColorIndex16& out_fg, ColorIndex16& out_bg) const
{
    out_glyph = phos::glyph::MakeUnicodeScalar(U' ');
    out_fg = kUnsetIndex16;
    out_bg = kUnsetIndex16;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_glyph = c.glyph;
    out_fg = c.fg;
    out_bg = c.bg;
    return true;
}

bool AnsiCanvas::GetCompositeCellPublicGlyphIndices(int row, int col, GlyphId& out_glyph, ColorIndex16& out_fg, ColorIndex16& out_bg, Attrs& out_attrs) const
{
    out_glyph = phos::glyph::MakeUnicodeScalar(U' ');
    out_fg = kUnsetIndex16;
    out_bg = kUnsetIndex16;
    out_attrs = 0;
    if (row < 0 || col < 0 || col >= m_columns || row >= m_rows)
        return false;
    CompositeCell c = GetCompositeCell(row, col);
    out_glyph = c.glyph;
    out_fg = c.fg;
    out_bg = c.bg;
    out_attrs = c.attrs;
    return true;
}

// ---- end inlined from canvas_core.inc ----


