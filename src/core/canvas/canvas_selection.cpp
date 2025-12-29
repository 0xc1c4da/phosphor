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

namespace
{
struct GlobalClipboard
{
    int w = 0;
    int h = 0;
    // Stored per-cell (same dimensions): glyph + fg + bg. 0 colours mean "unset".
    std::vector<AnsiCanvas::GlyphId> cp;
    std::vector<AnsiCanvas::ColourIndex16> fg;
    std::vector<AnsiCanvas::ColourIndex16> bg;
    std::vector<AnsiCanvas::Attrs> attrs;
};

// Shared across all canvases (translation-unit global).
static GlobalClipboard g_clipboard;

static inline AnsiCanvas::GlyphId BlankGlyph()
{
    return phos::glyph::MakeUnicodeScalar(U' ');
}
} // namespace

// ---- inlined from canvas_selection.inc ----
// ---------------------------------------------------------------------------
// Selection + clipboard
// ---------------------------------------------------------------------------

AnsiCanvas::Rect AnsiCanvas::GetSelectionRect() const
{
    Rect r;
    if (!HasSelection())
        return r;
    r.x = m_selection.x;
    r.y = m_selection.y;
    r.w = m_selection.w;
    r.h = m_selection.h;
    return r;
}

void AnsiCanvas::SetSelectionCorners(int x0, int y0, int x1, int y1)
{
    EnsureDocument();
    if (m_columns <= 0)
    {
        m_selection = SelectionState{};
        return;
    }

    x0 = std::clamp(x0, 0, m_columns - 1);
    x1 = std::clamp(x1, 0, m_columns - 1);
    if (y0 < 0) y0 = 0;
    if (y1 < 0) y1 = 0;

    const int minx = std::min(x0, x1);
    const int maxx = std::max(x0, x1);
    const int miny = std::min(y0, y1);
    const int maxy = std::max(y0, y1);

    const int w = (maxx - minx) + 1;
    const int h = (maxy - miny) + 1;
    if (w <= 0 || h <= 0)
    {
        m_selection = SelectionState{};
        return;
    }

    m_selection.active = true;
    m_selection.x = minx;
    m_selection.y = miny;
    m_selection.w = w;
    m_selection.h = h;
}

void AnsiCanvas::SelectAll()
{
    EnsureDocument();
    if (m_columns <= 0 || m_rows <= 0)
    {
        ClearSelection();
        return;
    }

    // Cancel any in-progress floating selection state and reset selection before expanding.
    ClearSelection();
    SetSelectionCorners(0, 0, m_columns - 1, m_rows - 1);
}

void AnsiCanvas::ClearSelection()
{
    m_selection = SelectionState{};
    if (m_move.active)
        m_move = MoveState{};
}

bool AnsiCanvas::SelectionContains(int x, int y) const
{
    if (!HasSelection())
        return false;
    if (x < m_selection.x || y < m_selection.y)
        return false;
    if (x >= m_selection.x + m_selection.w)
        return false;
    if (y >= m_selection.y + m_selection.h)
        return false;
    return true;
}

bool AnsiCanvas::ClipboardHas()
{
    if (g_clipboard.w <= 0 || g_clipboard.h <= 0)
        return false;
    const size_t n = (size_t)g_clipboard.w * (size_t)g_clipboard.h;
    return g_clipboard.cp.size() == n && g_clipboard.fg.size() == n && g_clipboard.bg.size() == n && g_clipboard.attrs.size() == n;
}

AnsiCanvas::Rect AnsiCanvas::ClipboardRect()
{
    Rect r;
    if (!ClipboardHas())
        return r;
    r.w = g_clipboard.w;
    r.h = g_clipboard.h;
    return r;
}

bool AnsiCanvas::CopySelectionToClipboard(int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.cp.assign(n, BlankGlyph());
    g_clipboard.fg.assign(n, kUnsetIndex16);
    g_clipboard.bg.assign(n, kUnsetIndex16);
    g_clipboard.attrs.assign(n, 0);

    const Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;
    for (int j = 0; j < h; ++j)
    {
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(y, x, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size())
                g_clipboard.cp[out] = layer.cells[idx];
            if (idx < layer.fg.size())
                g_clipboard.fg[out] = layer.fg[idx];
            if (idx < layer.bg.size())
                g_clipboard.bg[out] = layer.bg[idx];
            if (idx < layer.attrs.size())
                g_clipboard.attrs[out] = layer.attrs[idx];
        }
    }
    return true;
}

bool AnsiCanvas::CopySelectionToClipboardComposite()
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.cp.assign(n, BlankGlyph());
    g_clipboard.fg.assign(n, kUnsetIndex16);
    g_clipboard.bg.assign(n, kUnsetIndex16);
    g_clipboard.attrs.assign(n, 0);

    for (int j = 0; j < h; ++j)
    {
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            const CompositeCell c = GetCompositeCell(y, x);
            // Composite copy is intentionally lossy: store a Unicode representative in the clipboard glyph plane.
            g_clipboard.cp[out] = phos::glyph::MakeUnicodeScalar(c.cp);
            g_clipboard.fg[out] = c.fg;
            g_clipboard.bg[out] = c.bg;
            g_clipboard.attrs[out] = c.attrs;
        }
    }
    return true;
}

static bool ValidateBrushInternal(const AnsiCanvas::Brush& b)
{
    if (b.w <= 0 || b.h <= 0)
        return false;
    const size_t n = (size_t)b.w * (size_t)b.h;
    return b.cp.size() == n && b.fg.size() == n && b.bg.size() == n && b.attrs.size() == n;
}

bool AnsiCanvas::HasCurrentBrush() const
{
    return m_current_brush.has_value() && ValidateBrushInternal(*m_current_brush);
}

const AnsiCanvas::Brush* AnsiCanvas::GetCurrentBrush() const
{
    if (!HasCurrentBrush())
        return nullptr;
    return &(*m_current_brush);
}

void AnsiCanvas::ClearCurrentBrush()
{
    m_current_brush.reset();
}

bool AnsiCanvas::SetCurrentBrush(const Brush& brush)
{
    if (!ValidateBrushInternal(brush))
        return false;
    m_current_brush = brush;
    return true;
}

bool AnsiCanvas::CaptureBrushFromSelection(Brush& out, int layer_index)
{
    EnsureDocument();
    out = Brush{};
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    out.w = w;
    out.h = h;
    out.cp.assign(n, BlankGlyph());
    out.fg.assign(n, kUnsetIndex16);
    out.bg.assign(n, kUnsetIndex16);
    out.attrs.assign(n, 0);

    const Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out_idx = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(y, x, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size())
                out.cp[out_idx] = layer.cells[idx];
            if (idx < layer.fg.size())
                out.fg[out_idx] = layer.fg[idx];
            if (idx < layer.bg.size())
                out.bg[out_idx] = layer.bg[idx];
            if (idx < layer.attrs.size())
                out.attrs[out_idx] = layer.attrs[idx];
        }
    return ValidateBrushInternal(out);
}

bool AnsiCanvas::CaptureBrushFromSelectionComposite(Brush& out)
{
    EnsureDocument();
    out = Brush{};
    if (!HasSelection())
        return false;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    const size_t n = (size_t)w * (size_t)h;
    out.w = w;
    out.h = h;
    out.cp.assign(n, BlankGlyph());
    out.fg.assign(n, kUnsetIndex16);
    out.bg.assign(n, kUnsetIndex16);
    out.attrs.assign(n, 0);

    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const size_t out_idx = (size_t)j * (size_t)w + (size_t)i;

            if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                continue;

            const CompositeCell c = GetCompositeCell(y, x);
            // Composite capture flattens layers ("what you see" after visibility/blend/transparency),
            // but should remain glyph-token safe: store the composited GlyphId directly.
            // (CompositeCell::cp is only a best-effort Unicode representative and may lose token identity.)
            out.cp[out_idx] = c.glyph;
            out.fg[out_idx] = c.fg;
            out.bg[out_idx] = c.bg;
            out.attrs[out_idx] = c.attrs;
        }
    return ValidateBrushInternal(out);
}

bool AnsiCanvas::DeleteSelection(int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            if (x < 0 || x >= m_columns || y < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(y, x, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

            // If row is beyond current document, the old cell is implicitly transparent.
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = BlankGlyph();
            const ColourIndex16 new_fg = kUnsetIndex16;
            const ColourIndex16 new_bg = kUnsetIndex16;
            const Attrs    new_attrs = 0;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                              old_cp, old_fg, old_bg, old_attrs,
                                              new_cp, new_fg, new_bg, new_attrs))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue; // no-op

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);

            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size())
                layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())
                layer.fg[widx] = new_fg;
            if (widx < layer.bg.size())
                layer.bg[widx] = new_bg;
            if (widx < layer.attrs.size())
                layer.attrs[widx] = new_attrs;
            did_anything = true;
        }
    return did_anything;
}

bool AnsiCanvas::FlipSelectionX(int layer_index)
{
    EnsureDocument();
    if (IsMovingSelection())
        (void)CommitMoveSelection(layer_index);
    if (!HasSelection())
        return false;
    if (m_columns <= 0)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const Rect r = GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    std::vector<ClipCell> src;
    src.assign((size_t)r.w * (size_t)r.h, ClipCell{});

    // Capture source (token-safe).
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int sx = r.x + i;
            const int sy = r.y + j;
            const size_t out = (size_t)j * (size_t)r.w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size()) src[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())    src[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())    src[out].bg = layer.bg[idx];
            if (idx < layer.attrs.size()) src[out].attrs = layer.attrs[idx];
        }

    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int dx = r.x + i;
            const int dy = r.y + j;
            if (dx < 0 || dx >= m_columns || dy < 0)
                continue;

            const int si = (r.w - 1 - i);
            const size_t sidx = (size_t)j * (size_t)r.w + (size_t)si;
            if (sidx >= src.size())
                continue;
            const ClipCell& s = src[sidx];

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(dy, dx, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = s.cp;
            const ColourIndex16 new_fg = s.fg;
            const ColourIndex16 new_bg = s.bg;
            const Attrs    new_attrs = s.attrs;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }
    return did_anything;
}

bool AnsiCanvas::FlipSelectionY(int layer_index)
{
    EnsureDocument();
    if (IsMovingSelection())
        (void)CommitMoveSelection(layer_index);
    if (!HasSelection())
        return false;
    if (m_columns <= 0)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const Rect r = GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    std::vector<ClipCell> src;
    src.assign((size_t)r.w * (size_t)r.h, ClipCell{});

    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int sx = r.x + i;
            const int sy = r.y + j;
            const size_t out = (size_t)j * (size_t)r.w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size()) src[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())    src[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())    src[out].bg = layer.bg[idx];
            if (idx < layer.attrs.size()) src[out].attrs = layer.attrs[idx];
        }

    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int dx = r.x + i;
            const int dy = r.y + j;
            if (dx < 0 || dx >= m_columns || dy < 0)
                continue;

            const int sj = (r.h - 1 - j);
            const size_t sidx = (size_t)sj * (size_t)r.w + (size_t)i;
            if (sidx >= src.size())
                continue;
            const ClipCell& s = src[sidx];

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(dy, dx, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = s.cp;
            const ColourIndex16 new_fg = s.fg;
            const ColourIndex16 new_bg = s.bg;
            const Attrs    new_attrs = s.attrs;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }
    return did_anything;
}

static inline int RoundInt(float v)
{
    return (int)std::floor(v + 0.5f);
}

bool AnsiCanvas::RotateSelectionCw(int layer_index)
{
    EnsureDocument();
    if (IsMovingSelection())
        (void)CommitMoveSelection(layer_index);
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const Rect r = GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return false;

    const int new_w = r.h;
    const int new_h = r.w;
    if (m_columns <= 0 || new_w > m_columns)
        return false;

    // Rotate around center to minimize drift (match Lua policy).
    const float cx = (float)r.x + ((float)r.w - 1.0f) * 0.5f;
    const float cy = (float)r.y + ((float)r.h - 1.0f) * 0.5f;
    int nx = RoundInt(cx - ((float)new_w - 1.0f) * 0.5f);
    int ny = RoundInt(cy - ((float)new_h - 1.0f) * 0.5f);
    nx = std::clamp(nx, 0, std::max(0, m_columns - new_w));
    if (ny < 0) ny = 0;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    std::vector<ClipCell> src;
    src.assign((size_t)r.w * (size_t)r.h, ClipCell{});
    for (int oy = 0; oy < r.h; ++oy)
        for (int ox = 0; ox < r.w; ++ox)
        {
            const int sx = r.x + ox;
            const int sy = r.y + oy;
            const size_t out = (size_t)oy * (size_t)r.w + (size_t)ox;
            if (sx < 0 || sx >= m_columns || sy < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size()) src[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())    src[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())    src[out].bg = layer.bg[idx];
            if (idx < layer.attrs.size()) src[out].attrs = layer.attrs[idx];
        }

    std::vector<ClipCell> dst;
    dst.assign((size_t)new_w * (size_t)new_h, ClipCell{});
    for (int oy = 0; oy < r.h; ++oy)
        for (int ox = 0; ox < r.w; ++ox)
        {
            const size_t sidx = (size_t)oy * (size_t)r.w + (size_t)ox;
            const int dx = (r.h - 1 - oy);
            const int dy = ox;
            const size_t didx = (size_t)dy * (size_t)new_w + (size_t)dx;
            if (sidx < src.size() && didx < dst.size())
                dst[didx] = src[sidx];
        }

    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    // Clear old rect.
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int px = r.x + i;
            const int py = r.y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = BlankGlyph();
            const ColourIndex16 new_fg = kUnsetIndex16;
            const ColourIndex16 new_bg = kUnsetIndex16;
            const Attrs    new_attrs = 0;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;
            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    // Write rotated rect.
    for (int j = 0; j < new_h; ++j)
        for (int i = 0; i < new_w; ++i)
        {
            const int px = nx + i;
            const int py = ny + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;

            const size_t sidx = (size_t)j * (size_t)new_w + (size_t)i;
            if (sidx >= dst.size())
                continue;
            const ClipCell& s = dst[sidx];

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = s.cp;
            const ColourIndex16 new_fg = s.fg;
            const ColourIndex16 new_bg = s.bg;
            const Attrs    new_attrs = s.attrs;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;
            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    SetSelectionCorners(nx, ny, nx + new_w - 1, ny + new_h - 1);
    return did_anything;
}

bool AnsiCanvas::CenterSelection(int layer_index)
{
    EnsureDocument();
    if (IsMovingSelection())
        (void)CommitMoveSelection(layer_index);
    if (!HasSelection())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const Rect r = GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return false;
    if (m_columns <= 0)
        return false;

    int nx = 0;
    int ny = 0;
    if (m_columns > 0)
        nx = (m_columns - r.w) / 2;
    if (m_rows > 0)
        ny = (m_rows - r.h) / 2;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    nx = std::clamp(nx, 0, std::max(0, m_columns - r.w));

    if (nx == r.x && ny == r.y)
        return true; // no-op

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;

    std::vector<ClipCell> src;
    src.assign((size_t)r.w * (size_t)r.h, ClipCell{});
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int sx = r.x + i;
            const int sy = r.y + j;
            const size_t out = (size_t)j * (size_t)r.w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size()) src[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())    src[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())    src[out].bg = layer.bg[idx];
            if (idx < layer.attrs.size()) src[out].attrs = layer.attrs[idx];
        }

    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    // Clear old rect.
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int px = r.x + i;
            const int py = r.y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = BlankGlyph();
            const ColourIndex16 new_fg = kUnsetIndex16;
            const ColourIndex16 new_bg = kUnsetIndex16;
            const Attrs    new_attrs = 0;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;
            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    // Paste at new origin.
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i)
        {
            const int px = nx + i;
            const int py = ny + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            const size_t sidx = (size_t)j * (size_t)r.w + (size_t)i;
            if (sidx >= src.size())
                continue;
            const ClipCell& s = src[sidx];

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = s.cp;
            const ColourIndex16 new_fg = s.fg;
            const ColourIndex16 new_bg = s.bg;
            const Attrs    new_attrs = s.attrs;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                               old_cp, old_fg, old_bg, old_attrs,
                                               new_cp, new_fg, new_bg, new_attrs))
                continue;
            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);
            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size()) layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())    layer.fg[widx]    = new_fg;
            if (widx < layer.bg.size())    layer.bg[widx]    = new_bg;
            if (widx < layer.attrs.size()) layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    SetSelectionCorners(nx, ny, nx + r.w - 1, ny + r.h - 1);
    return did_anything;
}

bool AnsiCanvas::CropToSelection()
{
    EnsureDocument();
    if (IsMovingSelection())
        (void)CommitMoveSelection();
    if (!HasSelection())
        return false;

    const Rect r = GetSelectionRect();
    if (r.w <= 0 || r.h <= 0)
        return false;

    // Ensure this is one coherent undo step even if geometry doesn't change.
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    const int old_cols = m_columns;
    const int old_rows = m_rows;

    struct SavedLayer
    {
        std::vector<ClipCell> cells; // size r.w * r.h (row-major)
    };

    const int layer_count = GetLayerCount();
    std::vector<SavedLayer> saved;
    saved.resize((size_t)std::max(0, layer_count));
    const size_t n = (size_t)r.w * (size_t)r.h;
    for (int li = 0; li < layer_count; ++li)
    {
        SavedLayer s;
        s.cells.assign(n, ClipCell{});

        const Layer& layer = m_layers[(size_t)li];
        const int off_x = layer.offset_x;
        const int off_y = layer.offset_y;
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
            {
                const int sx = r.x + x;
                const int sy = r.y + y;
                const size_t idx_out = (size_t)y * (size_t)r.w + (size_t)x;
                if (sx < 0 || sx >= old_cols || sy < 0)
                    continue;

                int lr = 0, lc = 0;
                if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, old_cols, old_rows, lr, lc))
                    continue;

                const size_t idx = (size_t)lr * (size_t)old_cols + (size_t)lc;
                if (idx < layer.cells.size()) s.cells[idx_out].cp = layer.cells[idx];
                if (idx < layer.fg.size())    s.cells[idx_out].fg = layer.fg[idx];
                if (idx < layer.bg.size())    s.cells[idx_out].bg = layer.bg[idx];
                if (idx < layer.attrs.size()) s.cells[idx_out].attrs = layer.attrs[idx];
            }
        saved[(size_t)li] = std::move(s);
    }

    SetColumns(r.w);
    SetRows(r.h);

    // Crop is a structural "rebase" op: bake content into the new canvas coordinate space.
    // To avoid offset-induced out-of-bounds mapping after geometry changes, reset offsets.
    for (int li = 0; li < layer_count; ++li)
    {
        Layer& layer = m_layers[(size_t)li];
        layer.offset_x = 0;
        layer.offset_y = 0;
    }

    // Clear all layers, then restore saved region into new canvas coordinates.
    const size_t need = (size_t)std::max(0, m_rows) * (size_t)std::max(0, m_columns);
    for (int li = 0; li < layer_count; ++li)
    {
        Layer& layer = m_layers[(size_t)li];
        layer.cells.assign(need, BlankGlyph());
        layer.fg.assign(need, kUnsetIndex16);
        layer.bg.assign(need, kUnsetIndex16);
        layer.attrs.assign(need, 0);

        const SavedLayer& s = saved[(size_t)li];
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
            {
                const size_t src_idx = (size_t)y * (size_t)r.w + (size_t)x;
                if (src_idx >= s.cells.size())
                    continue;
                const ClipCell& cell = s.cells[src_idx];
                if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                    continue;
                const size_t dst_idx = (size_t)y * (size_t)m_columns + (size_t)x;
                if (dst_idx >= layer.cells.size())
                    continue;
                layer.cells[dst_idx] = cell.cp;
                if (dst_idx < layer.fg.size()) layer.fg[dst_idx] = cell.fg;
                if (dst_idx < layer.bg.size()) layer.bg[dst_idx] = cell.bg;
                if (dst_idx < layer.attrs.size()) layer.attrs[dst_idx] = cell.attrs;
            }
    }

    SetSelectionCorners(0, 0, r.w - 1, r.h - 1);
    return true;
}

bool AnsiCanvas::CutSelectionToClipboard(int layer_index)
{
    if (!CopySelectionToClipboard(layer_index))
        return false;
    return DeleteSelection(layer_index);
}

bool AnsiCanvas::PasteClipboard(int x, int y, int layer_index, PasteMode mode, bool transparent_spaces)
{
    EnsureDocument();
    if (!ClipboardHas())
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    const int w = g_clipboard.w;
    const int h = g_clipboard.h;
    if (w <= 0 || h <= 0)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int px = x + i;
            const int py = y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            if (!ToolWriteAllowed(py, px))
                continue;
            const size_t s = (size_t)j * (size_t)w + (size_t)i;
            if (s >= g_clipboard.cp.size())
                continue;

            const GlyphId cp = g_clipboard.cp[s];
            if (transparent_spaces && phos::glyph::IsBlank((phos::GlyphId)cp))
                continue;

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t dst = (size_t)lr * (size_t)m_columns + (size_t)lc;

            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && dst < layer.cells.size()) ? layer.cells[dst] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && dst < layer.fg.size()) ? layer.fg[dst] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && dst < layer.bg.size()) ? layer.bg[dst] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && dst < layer.attrs.size()) ? layer.attrs[dst] : 0;

            GlyphId  new_cp = old_cp;
            ColourIndex16 new_fg = old_fg;
            ColourIndex16 new_bg = old_bg;
            Attrs    new_attrs = old_attrs;
            if (mode == PasteMode::Both || mode == PasteMode::CharOnly)
                new_cp = cp;
            if (mode == PasteMode::Both || mode == PasteMode::ColourOnly)
            {
                new_fg = g_clipboard.fg[s];
                new_bg = g_clipboard.bg[s];
                new_attrs = g_clipboard.attrs[s];
            }

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                              old_cp, old_fg, old_bg, old_attrs,
                                              new_cp, new_fg, new_bg, new_attrs))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);

            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size())
                layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())
                layer.fg[widx] = new_fg;
            if (widx < layer.bg.size())
                layer.bg[widx] = new_bg;
            if (widx < layer.attrs.size())
                layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    SetSelectionCorners(x, y, x + w - 1, y + h - 1);
    return did_anything;
}

bool AnsiCanvas::BeginMoveSelection(int grab_x, int grab_y, bool copy, int layer_index)
{
    EnsureDocument();
    if (!HasSelection())
        return false;
    if (!SelectionContains(grab_x, grab_y))
        return false;
    if (m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    // Alpha-lock: disallow cutting (clearing source), but allow copying.
    // This matches typical "Lock Transparency" semantics (alpha cannot change).
    if (m_layers[(size_t)layer_index].lock_transparency)
        copy = true;

    const int x0 = m_selection.x;
    const int y0 = m_selection.y;
    const int w = m_selection.w;
    const int h = m_selection.h;
    if (w <= 0 || h <= 0)
        return false;

    MoveState mv;
    mv.active = true;
    mv.cut = !copy;
    mv.src_x = x0;
    mv.src_y = y0;
    mv.w = w;
    mv.h = h;
    mv.dst_x = x0;
    mv.dst_y = y0;
    mv.grab_dx = std::clamp(grab_x - x0, 0, std::max(0, w - 1));
    mv.grab_dy = std::clamp(grab_y - y0, 0, std::max(0, h - 1));
    mv.cells.assign((size_t)w * (size_t)h, ClipCell{});

    const Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int sx = x0 + i;
            const int sy = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0 || sy >= m_rows)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForReadFast(sy, sx, off_x, off_y, m_columns, m_rows, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size())
                mv.cells[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())
                mv.cells[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())
                mv.cells[out].bg = layer.bg[idx];
            if (idx < layer.attrs.size())
                mv.cells[out].attrs = layer.attrs[idx];
        }

    if (mv.cut)
    {
        Layer& mut = m_layers[(size_t)layer_index];
        const int mut_off_x = mut.offset_x;
        const int mut_off_y = mut.offset_y;
        bool prepared = false;
        auto prepare = [&]()
        {
            if (!prepared)
            {
                PrepareUndoForMutation();
                EnsureUndoCaptureIsPatch();
                prepared = true;
            }
        };
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
            {
                const int sx = x0 + i;
                const int sy = y0 + j;
                if (sx < 0 || sx >= m_columns || sy < 0)
                    continue;
                int lr = 0, lc = 0;
                if (!CanvasToLayerLocalForWriteFast(sy, sx, mut_off_x, mut_off_y, m_columns, lr, lc))
                    continue;
                const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

                const bool in_bounds = (lr < m_rows);
                const GlyphId  old_cp = (in_bounds && idx < mut.cells.size()) ? mut.cells[idx] : BlankGlyph();
                const ColourIndex16 old_fg = (in_bounds && idx < mut.fg.size()) ? mut.fg[idx] : kUnsetIndex16;
                const ColourIndex16 old_bg = (in_bounds && idx < mut.bg.size()) ? mut.bg[idx] : kUnsetIndex16;
                const Attrs    old_attrs = (in_bounds && idx < mut.attrs.size()) ? mut.attrs[idx] : 0;
                const GlyphId  new_cp = BlankGlyph();
                const ColourIndex16 new_fg = kUnsetIndex16;
                const ColourIndex16 new_bg = kUnsetIndex16;
                const Attrs    new_attrs = 0;

                if (!TransparencyTransitionAllowed(mut.lock_transparency,
                                                  old_cp, old_fg, old_bg, old_attrs,
                                                  new_cp, new_fg, new_bg, new_attrs))
                    continue;

                if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                    continue;

                prepare();
                CaptureUndoPageIfNeeded(layer_index, lr);
                if (lr >= m_rows)
                    EnsureRows(lr + 1);

                const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
                if (widx < mut.cells.size())
                    mut.cells[widx] = new_cp;
                if (widx < mut.fg.size())
                    mut.fg[widx] = new_fg;
                if (widx < mut.bg.size())
                    mut.bg[widx] = new_bg;
                if (widx < mut.attrs.size())
                    mut.attrs[widx] = new_attrs;
            }
    }

    m_move = std::move(mv);
    return true;
}

void AnsiCanvas::UpdateMoveSelection(int cursor_x, int cursor_y)
{
    if (!m_move.active)
        return;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    const int nx = cursor_x - m_move.grab_dx;
    const int ny = cursor_y - m_move.grab_dy;
    m_move.dst_x = std::clamp(nx, 0, std::max(0, m_columns - 1));
    m_move.dst_y = std::max(0, ny);
    SetSelectionCorners(m_move.dst_x, m_move.dst_y,
                        m_move.dst_x + m_move.w - 1,
                        m_move.dst_y + m_move.h - 1);
}

bool AnsiCanvas::CommitMoveSelection(int layer_index)
{
    EnsureDocument();
    if (!m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    const int w = m_move.w;
    const int h = m_move.h;
    if (w <= 0 || h <= 0 || (int)m_move.cells.size() != w * h)
        return false;

    Layer& layer = m_layers[(size_t)layer_index];
    const int off_x = layer.offset_x;
    const int off_y = layer.offset_y;
    bool did_anything = false;
    bool prepared = false;
    auto prepare = [&]()
    {
        if (!prepared)
        {
            PrepareUndoForMutation();
            EnsureUndoCaptureIsPatch();
            prepared = true;
        }
    };

    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int px = m_move.dst_x + i;
            const int py = m_move.dst_y + j;
            if (px < 0 || px >= m_columns || py < 0)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

            const bool in_bounds = (lr < m_rows);
            const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
            const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
            const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
            const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

            const GlyphId  new_cp = src.cp;
            const ColourIndex16 new_fg = src.fg;
            const ColourIndex16 new_bg = src.bg;
            const Attrs    new_attrs = src.attrs;

            if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                              old_cp, old_fg, old_bg, old_attrs,
                                              new_cp, new_fg, new_bg, new_attrs))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                continue;

            prepare();
            CaptureUndoPageIfNeeded(layer_index, lr);
            if (lr >= m_rows)
                EnsureRows(lr + 1);

            const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (widx < layer.cells.size())
                layer.cells[widx] = new_cp;
            if (widx < layer.fg.size())
                layer.fg[widx] = new_fg;
            if (widx < layer.bg.size())
                layer.bg[widx] = new_bg;
            if (widx < layer.attrs.size())
                layer.attrs[widx] = new_attrs;
            did_anything = true;
        }

    SetSelectionCorners(m_move.dst_x, m_move.dst_y,
                        m_move.dst_x + w - 1,
                        m_move.dst_y + h - 1);
    m_move = MoveState{};
    return did_anything;
}

bool AnsiCanvas::CancelMoveSelection(int layer_index)
{
    EnsureDocument();
    if (!m_move.active)
        return false;

    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (m_move.cut)
    {
        const int w = m_move.w;
        const int h = m_move.h;
        if (w > 0 && h > 0 && (int)m_move.cells.size() == w * h)
        {
            Layer& layer = m_layers[(size_t)layer_index];
            const int off_x = layer.offset_x;
            const int off_y = layer.offset_y;
            bool did_anything = false;
            bool prepared = false;
            auto prepare = [&]()
            {
                if (!prepared)
                {
                    PrepareUndoForMutation();
                    EnsureUndoCaptureIsPatch();
                    prepared = true;
                }
            };
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                {
                    const int px = m_move.src_x + i;
                    const int py = m_move.src_y + j;
                    if (px < 0 || px >= m_columns || py < 0)
                        continue;
                    int lr = 0, lc = 0;
                    if (!CanvasToLayerLocalForWriteFast(py, px, off_x, off_y, m_columns, lr, lc))
                        continue;
                    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
                    const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

                    const bool in_bounds = (lr < m_rows);
                    const GlyphId  old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : BlankGlyph();
                    const ColourIndex16 old_fg = (in_bounds && idx < layer.fg.size()) ? layer.fg[idx] : kUnsetIndex16;
                    const ColourIndex16 old_bg = (in_bounds && idx < layer.bg.size()) ? layer.bg[idx] : kUnsetIndex16;
                    const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

                    const GlyphId  new_cp = src.cp;
                    const ColourIndex16 new_fg = src.fg;
                    const ColourIndex16 new_bg = src.bg;
                    const Attrs    new_attrs = src.attrs;

                    if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                                      old_cp, old_fg, old_bg, old_attrs,
                                                      new_cp, new_fg, new_bg, new_attrs))
                        continue;

                    if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
                        continue;

                    prepare();
                    CaptureUndoPageIfNeeded(layer_index, lr);
                    if (lr >= m_rows)
                        EnsureRows(lr + 1);

                    const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
                    if (widx < layer.cells.size())
                        layer.cells[widx] = new_cp;
                    if (widx < layer.fg.size())
                        layer.fg[widx] = new_fg;
                    if (widx < layer.bg.size())
                        layer.bg[widx] = new_bg;
                    if (widx < layer.attrs.size())
                        layer.attrs[widx] = new_attrs;
                    did_anything = true;
                }
            (void)did_anything;
        }
    }

    SetSelectionCorners(m_move.src_x, m_move.src_y,
                        m_move.src_x + m_move.w - 1,
                        m_move.src_y + m_move.h - 1);
    m_move = MoveState{};
    return true;
}

void AnsiCanvas::SetCaretCell(int x, int y)
{
    EnsureDocument();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= m_columns) x = m_columns - 1;
    m_caret_col = x;
    m_caret_row = y;
    EnsureRows(m_caret_row + 1);
}

// Legacy hidden ImGui InputText path removed (text input is routed host-side via SDL_EVENT_TEXT_INPUT).

// NOTE: Canvas key polling (CaptureKeyEvents) has been removed.
// Per-frame key events are computed by the host and injected via SetKeyEventsForFrame().

// ---- end inlined from canvas_selection.inc ----


