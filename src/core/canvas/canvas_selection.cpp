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
    // Stored per-cell (same dimensions): glyph + fg + bg. 0 colors mean "unset".
    std::vector<char32_t> cp;
    std::vector<AnsiCanvas::Color32> fg;
    std::vector<AnsiCanvas::Color32> bg;
};

// Shared across all canvases (translation-unit global).
static GlobalClipboard g_clipboard;
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
    return g_clipboard.cp.size() == n && g_clipboard.fg.size() == n && g_clipboard.bg.size() == n;
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
    g_clipboard.cp.assign(n, U' ');
    g_clipboard.fg.assign(n, 0);
    g_clipboard.bg.assign(n, 0);

    const Layer& layer = m_layers[(size_t)layer_index];
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
            if (!CanvasToLayerLocalForRead(layer_index, y, x, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size())
                g_clipboard.cp[out] = layer.cells[idx];
            if (idx < layer.fg.size())
                g_clipboard.fg[out] = layer.fg[idx];
            if (idx < layer.bg.size())
                g_clipboard.bg[out] = layer.bg[idx];
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
    g_clipboard.cp.assign(n, U' ');
    g_clipboard.fg.assign(n, 0);
    g_clipboard.bg.assign(n, 0);

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
            g_clipboard.cp[out] = c.cp;
            g_clipboard.fg[out] = c.fg;
            g_clipboard.bg[out] = c.bg;
        }
    }
    return true;
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
            if (!CanvasToLayerLocalForWrite(layer_index, y, x, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

            // If row is beyond current document, the old cell is implicitly transparent.
            const bool in_bounds = (lr < m_rows);
            const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
            const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
            const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

            const char32_t new_cp = U' ';
            const Color32  new_fg = 0;
            const Color32  new_bg = 0;

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
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
            did_anything = true;
        }
    return did_anything;
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
            const size_t s = (size_t)j * (size_t)w + (size_t)i;
            if (s >= g_clipboard.cp.size())
                continue;

            const char32_t cp = g_clipboard.cp[s];
            if (transparent_spaces && cp == U' ')
                continue;

            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForWrite(layer_index, py, px, lr, lc))
                continue;
            const size_t dst = (size_t)lr * (size_t)m_columns + (size_t)lc;

            const bool in_bounds = (lr < m_rows);
            const char32_t old_cp = (in_bounds && dst < layer.cells.size()) ? layer.cells[dst] : U' ';
            const Color32  old_fg = (in_bounds && dst < layer.fg.size())    ? layer.fg[dst]    : 0;
            const Color32  old_bg = (in_bounds && dst < layer.bg.size())    ? layer.bg[dst]    : 0;

            char32_t new_cp = old_cp;
            Color32  new_fg = old_fg;
            Color32  new_bg = old_bg;
            if (mode == PasteMode::Both || mode == PasteMode::CharOnly)
                new_cp = cp;
            if (mode == PasteMode::Both || mode == PasteMode::ColorOnly)
            {
                new_fg = g_clipboard.fg[s];
                new_bg = g_clipboard.bg[s];
            }

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
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
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
        {
            const int sx = x0 + i;
            const int sy = y0 + j;
            const size_t out = (size_t)j * (size_t)w + (size_t)i;
            if (sx < 0 || sx >= m_columns || sy < 0 || sy >= m_rows)
                continue;
            int lr = 0, lc = 0;
            if (!CanvasToLayerLocalForRead(layer_index, sy, sx, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            if (idx < layer.cells.size())
                mv.cells[out].cp = layer.cells[idx];
            if (idx < layer.fg.size())
                mv.cells[out].fg = layer.fg[idx];
            if (idx < layer.bg.size())
                mv.cells[out].bg = layer.bg[idx];
        }

    if (mv.cut)
    {
        Layer& mut = m_layers[(size_t)layer_index];
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
                if (!CanvasToLayerLocalForWrite(layer_index, sy, sx, lr, lc))
                    continue;
                const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

                const bool in_bounds = (lr < m_rows);
                const char32_t old_cp = (in_bounds && idx < mut.cells.size()) ? mut.cells[idx] : U' ';
                const Color32  old_fg = (in_bounds && idx < mut.fg.size())    ? mut.fg[idx]    : 0;
                const Color32  old_bg = (in_bounds && idx < mut.bg.size())    ? mut.bg[idx]    : 0;
                const char32_t new_cp = U' ';
                const Color32  new_fg = 0;
                const Color32  new_bg = 0;

                if (!TransparencyTransitionAllowed(mut.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                    continue;

                if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
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
            if (!CanvasToLayerLocalForWrite(layer_index, py, px, lr, lc))
                continue;
            const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
            const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

            const bool in_bounds = (lr < m_rows);
            const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
            const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
            const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

            const char32_t new_cp = src.cp;
            const Color32  new_fg = src.fg;
            const Color32  new_bg = src.bg;

            if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                continue;

            if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
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
                    if (!CanvasToLayerLocalForWrite(layer_index, py, px, lr, lc))
                        continue;
                    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
                    const ClipCell& src = m_move.cells[(size_t)j * (size_t)w + (size_t)i];

                    const bool in_bounds = (lr < m_rows);
                    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
                    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
                    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;

                    const char32_t new_cp = src.cp;
                    const Color32  new_fg = src.fg;
                    const Color32  new_bg = src.bg;

                    if (!TransparencyTransitionAllowed(layer.lock_transparency, old_cp, old_fg, old_bg, new_cp, new_fg, new_bg))
                        continue;

                    if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
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

int AnsiCanvas::TextInputCallback(ImGuiInputTextCallbackData* data)
{
    if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter)
        return 0;

    AnsiCanvas* self = static_cast<AnsiCanvas*>(data->UserData);
    if (!self)
        return 0;

    const char32_t cp = static_cast<char32_t>(data->EventChar);
    // Queue typed codepoints so the active tool (ANSL) can implement editing behavior.
    self->m_typed_queue.push_back(cp);

    // We applied the character to the canvas; don't let InputText mutate its own buffer.
    return 1;
}

void AnsiCanvas::HandleCharInputWidget(const char* id)
{
    if (!id)
        return;

    // If the user is editing the status bar (Cols/Rows/Caret), don't run the hidden text input
    // widget at all. This prevents it from competing for ActiveId / keyboard focus.
    if (m_status_bar_editing)
        return;

    // SDL3 backend only emits text input events when ImGui indicates it wants text input.
    // The most robust way to do that is to keep a focused InputText widget.
    // We render it "invisible" and use a char-filter callback to apply typed characters
    // directly into the canvas cells.
    std::string input_id = std::string(id) + "##_text_input";

    // Tiny dummy buffer. All characters are filtered out by the callback, so it stays empty.
    static char dummy[2] = { 0, 0 };

    // Make the widget visually invisible but still interactive.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextItemWidth(1.0f);

    ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_CallbackCharFilter |
        ImGuiInputTextFlags_NoUndoRedo |
        ImGuiInputTextFlags_AlwaysOverwrite |
        ImGuiInputTextFlags_AllowTabInput |
        ImGuiInputTextFlags_EnterReturnsTrue;

    // Keep keyboard focus on this widget while the canvas is focused.
    //
    // SDL3 backend only emits text input events when ImGui indicates it wants text input,
    // so we need a focused InputText to receive characters.
    //
    // IMPORTANT: avoid stealing ActiveId from other windows.
    //
    // The tool palette (and other tool windows) render before canvases each frame.
    // If we call SetKeyboardFocusHere() later in the frame while the user is clicking
    // another window, this hidden InputText can steal ActiveId and make that click
    // appear to "not work" (often requiring a second click).
    //
    // Therefore we only refocus while:
    // - the canvas is logically focused
    // - the canvas window is focused *and hovered* (mouse is actually over it)
    // - no mouse interaction happened this frame
    // - no popup is open
    ImGuiIO& io = ImGui::GetIO();
    const bool any_mouse_down =
        io.MouseDown[ImGuiMouseButton_Left] ||
        io.MouseDown[ImGuiMouseButton_Right] ||
        io.MouseDown[ImGuiMouseButton_Middle];
    const bool any_mouse_click =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const bool any_mouse_release =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Middle);
    const bool any_mouse_interaction = any_mouse_down || any_mouse_click || any_mouse_release;

    // Avoid stealing focus from other UI elements (including our own status-line fields).
    // If another widget is active, don't force focus back to the hidden InputText.
    const ImGuiID hidden_id = ImGui::GetID(input_id.c_str());
    const ImGuiID active_id = ImGui::GetActiveID();
    const bool other_widget_active = (active_id != 0 && active_id != hidden_id);

    if (m_has_focus &&
        !other_widget_active &&
        !any_mouse_interaction &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::InputText(input_id.c_str(), dummy, IM_ARRAYSIZE(dummy), flags, &AnsiCanvas::TextInputCallback, this);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void AnsiCanvas::CaptureKeyEvents()
{
    m_key_events = KeyEvents{};
    if (!m_has_focus)
        return;
    // Bind keyboard navigation to *ImGui window focus* (not just our internal canvas focus).
    // Otherwise arrow keys pressed while interacting with other windows (e.g. character picker)
    // can still be consumed by the canvas because IsKeyPressed() is global.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    // If a popup/modal is open, don't interpret keys as canvas commands.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    // Match previous behavior: discrete press events.
    //
    // If a key bindings engine is attached, resolve navigation/edit keys through action IDs
    // so tools/scripts can be remapped without editing Lua.
    if (m_keybinds)
    {
        kb::EvalContext kctx;
        kctx.global = true;
        kctx.editor = true;
        kctx.canvas = true;
        kctx.selection = HasSelection();
        kctx.platform = kb::RuntimePlatform();

        m_key_events.left  = m_keybinds->ActionPressed("nav.caret_left", kctx);
        m_key_events.right = m_keybinds->ActionPressed("nav.caret_right", kctx);
        m_key_events.up    = m_keybinds->ActionPressed("nav.caret_up", kctx);
        m_key_events.down  = m_keybinds->ActionPressed("nav.caret_down", kctx);
        m_key_events.home  = m_keybinds->ActionPressed("nav.home", kctx);
        m_key_events.end   = m_keybinds->ActionPressed("nav.end", kctx);

        m_key_events.backspace = m_keybinds->ActionPressed("editor.backspace", kctx);

        // "Delete" is ambiguous: when selection exists, treat it as "delete selection";
        // otherwise allow the (optional) forward-delete editor action.
        if (kctx.selection)
            m_key_events.del = m_keybinds->ActionPressed("selection.delete", kctx);
        else
            m_key_events.del = m_keybinds->ActionPressed("editor.delete_forward", kctx);

        m_key_events.enter = m_keybinds->ActionPressed("editor.new_line", kctx);
    }
    else
    {
        m_key_events.left      = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        m_key_events.right     = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        m_key_events.up        = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        m_key_events.down      = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        m_key_events.home      = ImGui::IsKeyPressed(ImGuiKey_Home);
        m_key_events.end       = ImGui::IsKeyPressed(ImGuiKey_End);
        m_key_events.backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace);
        m_key_events.del       = ImGui::IsKeyPressed(ImGuiKey_Delete);
        m_key_events.enter     = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    }

    // Selection/clipboard keys (used by tools; modifiers are checked separately via ImGuiIO in the host).
    m_key_events.c      = ImGui::IsKeyPressed(ImGuiKey_C, false);
    m_key_events.v      = ImGui::IsKeyPressed(ImGuiKey_V, false);
    m_key_events.x      = ImGui::IsKeyPressed(ImGuiKey_X, false);
    m_key_events.a      = ImGui::IsKeyPressed(ImGuiKey_A, false);
    m_key_events.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

// ---- end inlined from canvas_selection.inc ----


