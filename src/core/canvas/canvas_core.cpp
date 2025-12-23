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

// ---- end inlined from canvas_core.inc ----


