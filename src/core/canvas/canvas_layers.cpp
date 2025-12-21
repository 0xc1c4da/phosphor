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

// ---- inlined from canvas_layers.inc ----
int AnsiCanvas::GetLayerCount() const
{
    return static_cast<int>(m_layers.size());
}

int AnsiCanvas::GetActiveLayerIndex() const
{
    return m_active_layer;
}

std::string AnsiCanvas::GetLayerName(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return {};
    return m_layers[static_cast<size_t>(index)].name;
}

bool AnsiCanvas::IsLayerVisible(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    return m_layers[static_cast<size_t>(index)].visible;
}

bool AnsiCanvas::IsLayerTransparencyLocked(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    return m_layers[static_cast<size_t>(index)].lock_transparency;
}

bool AnsiCanvas::SetLayerName(int index, const std::string& name)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].name == name)
        return true; // no-op
    // Structural change: if we're not inside the canvas render's undo capture for this frame
    // (e.g. UI panels mutate the canvas before AnsiCanvas::Render runs), still make it undoable.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_layers[static_cast<size_t>(index)].name = name;
    return true;
}

int AnsiCanvas::AddLayer(const std::string& name)
{
    EnsureDocument();
    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    Layer layer;
    layer.name = name.empty() ? ("Layer " + std::to_string((int)m_layers.size() + 1)) : name;
    layer.visible = true;
    const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    layer.cells.assign(count, U' ');
    layer.fg.assign(count, 0);
    layer.bg.assign(count, 0);
    layer.attrs.assign(count, 0);

    m_layers.push_back(std::move(layer));
    m_active_layer = static_cast<int>(m_layers.size()) - 1;
    return m_active_layer;
}

bool AnsiCanvas::RemoveLayer(int index)
{
    EnsureDocument();
    if (m_layers.size() <= 1)
        return false; // must keep at least one layer
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;

    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_layers.erase(m_layers.begin() + index);
    if (m_active_layer >= static_cast<int>(m_layers.size()))
        m_active_layer = static_cast<int>(m_layers.size()) - 1;
    if (m_active_layer < 0)
        m_active_layer = 0;
    return true;
}

bool AnsiCanvas::SetActiveLayerIndex(int index)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    m_active_layer = index;
    return true;
}

bool AnsiCanvas::SetLayerVisible(int index, bool visible)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    if (m_layers[static_cast<size_t>(index)].visible == visible)
        return true;
    m_layers[static_cast<size_t>(index)].visible = visible;
    TouchContent();
    return true;
}

bool AnsiCanvas::SetLayerTransparencyLocked(int index, bool locked)
{
    EnsureDocument();
    if (index < 0 || index >= static_cast<int>(m_layers.size()))
        return false;
    m_layers[static_cast<size_t>(index)].lock_transparency = locked;
    return true;
}

bool AnsiCanvas::MoveLayer(int from_index, int to_index)
{
    EnsureDocument();
    const int n = static_cast<int>(m_layers.size());
    if (from_index < 0 || from_index >= n)
        return false;
    if (to_index < 0 || to_index >= n)
        return false;
    if (from_index == to_index)
        return true;

    // Structural change: ensure this is undoable even if invoked outside an active capture.
    if (!m_undo_capture_active)
        PushUndoSnapshot();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    Layer moving = std::move(m_layers[(size_t)from_index]);
    m_layers.erase(m_layers.begin() + from_index);
    m_layers.insert(m_layers.begin() + to_index, std::move(moving));

    // Keep active layer pointing at the same logical layer.
    if (m_active_layer == from_index)
        m_active_layer = to_index;
    else if (from_index < to_index)
    {
        // Elements in (from_index, to_index] shift left by 1.
        if (m_active_layer > from_index && m_active_layer <= to_index)
            m_active_layer -= 1;
    }
    else // from_index > to_index
    {
        // Elements in [to_index, from_index) shift right by 1.
        if (m_active_layer >= to_index && m_active_layer < from_index)
            m_active_layer += 1;
    }

    if (m_active_layer < 0) m_active_layer = 0;
    if (m_active_layer >= (int)m_layers.size()) m_active_layer = (int)m_layers.size() - 1;
    return true;
}

bool AnsiCanvas::MoveLayerUp(int index)
{
    return MoveLayer(index, index + 1);
}

bool AnsiCanvas::MoveLayerDown(int index)
{
    return MoveLayer(index, index - 1);
}

bool AnsiCanvas::GetLayerOffset(int layer_index, int& out_x, int& out_y) const
{
    out_x = 0;
    out_y = 0;
    if (m_layers.empty())
        return false;
    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    out_x = layer.offset_x;
    out_y = layer.offset_y;
    return true;
}

bool AnsiCanvas::SetLayerOffset(int x, int y, int layer_index)
{
    EnsureDocument();
    layer_index = NormalizeLayerIndex(*this, layer_index);
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    Layer& layer = m_layers[(size_t)layer_index];
    if (layer.offset_x == x && layer.offset_y == y)
        return true;
    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();
    layer.offset_x = x;
    layer.offset_y = y;
    return true;
}

bool AnsiCanvas::NudgeLayerOffset(int dx, int dy, int layer_index)
{
    int x = 0, y = 0;
    if (!GetLayerOffset(layer_index, x, y))
        return false;
    return SetLayerOffset(x + dx, y + dy, layer_index);
}

void AnsiCanvas::SetColumns(int columns)
{
    if (columns <= 0)
        return;
    if (columns > 4096)
        columns = 4096;
    EnsureDocument();

    if (columns == m_columns)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    const int old_cols = m_columns;
    const int old_rows = m_rows;
    m_columns = columns;

    for (Layer& layer : m_layers)
    {
        std::vector<char32_t> new_cells;
        std::vector<Color32>  new_fg;
        std::vector<Color32>  new_bg;
        std::vector<Attrs>    new_attrs;

        new_cells.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), U' ');
        new_fg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);
        new_bg.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);
        new_attrs.assign(static_cast<size_t>(old_rows) * static_cast<size_t>(m_columns), 0);

        const int copy_cols = std::min(old_cols, m_columns);
        for (int r = 0; r < old_rows; ++r)
        {
            for (int c = 0; c < copy_cols; ++c)
            {
                const size_t src = static_cast<size_t>(r) * static_cast<size_t>(old_cols) + static_cast<size_t>(c);
                const size_t dst = static_cast<size_t>(r) * static_cast<size_t>(m_columns) + static_cast<size_t>(c);
                if (src < layer.cells.size() && dst < new_cells.size())
                    new_cells[dst] = layer.cells[src];
                if (src < layer.fg.size() && dst < new_fg.size())
                    new_fg[dst] = layer.fg[src];
                if (src < layer.bg.size() && dst < new_bg.size())
                    new_bg[dst] = layer.bg[src];
                if (src < layer.attrs.size() && dst < new_attrs.size())
                    new_attrs[dst] = layer.attrs[src];
            }
        }

        layer.cells = std::move(new_cells);
        layer.fg    = std::move(new_fg);
        layer.bg    = std::move(new_bg);
        layer.attrs = std::move(new_attrs);
    }

    // Clamp cursor to new width.
    if (m_caret_col >= m_columns)
        m_caret_col = m_columns - 1;
    if (m_caret_col < 0)
        m_caret_col = 0;

    // If a floating move is active, cancel it (cropping/resize is simpler than re-mapping).
    if (m_move.active)
    {
        m_move = MoveState{};
        m_selection = SelectionState{};
    }
    else if (HasSelection())
    {
        // Clamp selection to new bounds.
        const int max_x = m_columns - 1;
        const int max_y = m_rows - 1;
        if (max_x < 0 || max_y < 0)
        {
            m_selection = SelectionState{};
        }
        else
        {
            int x0 = m_selection.x;
            int y0 = m_selection.y;
            int x1 = m_selection.x + m_selection.w - 1;
            int y1 = m_selection.y + m_selection.h - 1;
            x0 = std::clamp(x0, 0, max_x);
            x1 = std::clamp(x1, 0, max_x);
            y0 = std::clamp(y0, 0, max_y);
            y1 = std::clamp(y1, 0, max_y);
            if (x1 < x0 || y1 < y0)
                m_selection = SelectionState{};
            else
                SetSelectionCorners(x0, y0, x1, y1);
        }
    }

    // Keep SAUCE metadata consistent with the document geometry.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

void AnsiCanvas::SetRows(int rows)
{
    if (rows <= 0)
        return;
    EnsureDocument();

    if (rows == m_rows)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();
    m_rows = rows;

    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        layer.cells.resize(need, U' ');
        layer.fg.resize(need, 0);
        layer.bg.resize(need, 0);
        layer.attrs.resize(need, 0);
    }

    // Clamp caret to new height.
    if (m_caret_row >= m_rows)
        m_caret_row = m_rows - 1;
    if (m_caret_row < 0)
        m_caret_row = 0;

    // Cancel an active floating move (simpler/safer than trying to crop the in-flight payload).
    if (m_move.active)
    {
        m_move = MoveState{};
        m_selection = SelectionState{};
    }
    else if (HasSelection())
    {
        // Clamp selection to new bounds.
        const int max_x = m_columns - 1;
        const int max_y = m_rows - 1;
        if (max_x < 0 || max_y < 0)
        {
            m_selection = SelectionState{};
        }
        else
        {
            int x0 = m_selection.x;
            int y0 = m_selection.y;
            int x1 = m_selection.x + m_selection.w - 1;
            int y1 = m_selection.y + m_selection.h - 1;
            x0 = std::clamp(x0, 0, max_x);
            x1 = std::clamp(x1, 0, max_x);
            y0 = std::clamp(y0, 0, max_y);
            y1 = std::clamp(y1, 0, max_y);
            if (x1 < x0 || y1 < y0)
                m_selection = SelectionState{};
            else
                SetSelectionCorners(x0, y0, x1, y1);
        }
    }

    // Keep SAUCE metadata consistent with the document geometry.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

bool AnsiCanvas::LoadFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "AnsiCanvas: failed to open '%s'\n", path.c_str());
        return false;
    }

    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());

    EnsureDocument();
    PrepareUndoForMutation();
    EnsureUndoCaptureIsSnapshot();

    // Reset document to a single empty row.
    m_rows = 1;
    for (Layer& layer : m_layers)
    {
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        layer.cells.assign(count, U' ');
        layer.fg.assign(count, 0);
        layer.bg.assign(count, 0);
        layer.attrs.assign(count, 0);
    }

    std::vector<char32_t> cps;
    DecodeUtf8(bytes, cps);

    int row = 0;
    int col = 0;
    bool last_was_cr = false;

    for (char32_t cp : cps)
    {
        // Normalize CRLF.
        if (cp == U'\r')
        {
            last_was_cr = true;
            row++;
            col = 0;
            EnsureRows(row + 1);
            continue;
        }
        if (cp == U'\n')
        {
            if (last_was_cr)
            {
                last_was_cr = false;
                continue;
            }
            row++;
            col = 0;
            EnsureRows(row + 1);
            continue;
        }
        last_was_cr = false;

        // Filter control chars for now (ANSI parsing will come later).
        if (cp == U'\t')
            cp = U' ';
        if (cp < 0x20)
            continue;

        SetActiveCell(row, col, cp);
        col++;
        if (col >= m_columns)
        {
            row++;
            col = 0;
            EnsureRows(row + 1);
        }
    }

    m_caret_row = 0;
    m_caret_col = 0;

    // Loaded content establishes a concrete geometry; reflect it in SAUCE.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
    return true;
}

void AnsiCanvas::EnsureDocument()
{
    if (m_columns <= 0)
        m_columns = 80;
    if (m_rows <= 0)
        m_rows = 1;

    if (m_layers.empty())
    {
        Layer base;
        base.name = "Base";
        base.visible = true;
        const size_t count = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
        base.cells.assign(count, U' ');
        base.fg.assign(count, 0);
        base.bg.assign(count, 0);
        base.attrs.assign(count, 0);
        m_layers.push_back(std::move(base));
        m_active_layer = 0;
    }

    // Ensure every layer has the correct cell count.
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        if (layer.cells.size() != need)
            layer.cells.resize(need, U' ');
        if (layer.fg.size() != need)
            layer.fg.resize(need, 0);
        if (layer.bg.size() != need)
            layer.bg.resize(need, 0);
        if (layer.attrs.size() != need)
            layer.attrs.resize(need, 0);
    }

    if (m_active_layer < 0)
        m_active_layer = 0;
    if (m_active_layer >= (int)m_layers.size())
        m_active_layer = (int)m_layers.size() - 1;

    // Ensure SAUCE defaults exist even for canvases created via bare constructor.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

void AnsiCanvas::EnsureRows(int rows_needed)
{
    if (rows_needed <= 0)
        rows_needed = 1;

    EnsureDocument();
    if (rows_needed <= m_rows)
        return;

    PrepareUndoForMutation();
    EnsureUndoCaptureIsPatch();
    m_rows = rows_needed;
    const size_t need = static_cast<size_t>(m_rows) * static_cast<size_t>(m_columns);
    for (Layer& layer : m_layers)
    {
        // Growing one row at a time (common during mouse painting downward) can cause many
        // expensive reallocations/copies on large canvases. Reserve a modest amount of slack
        // capacity so repeated EnsureRows() calls are amortized, without changing `m_rows`
        // (visible canvas size) or any behavior.
        auto reserve_with_slack = [&](auto& v)
        {
            if (need <= v.size())
                return;
            if (need <= v.capacity())
                return;
            // Slack heuristic: ~12.5% extra, or ~64 rows worth of cells (whichever larger).
            const size_t row_chunk = static_cast<size_t>(std::max(1, m_columns)) * static_cast<size_t>(64);
            const size_t frac = need / 8;
            size_t slack = std::max(row_chunk, frac);
            // Avoid overflow.
            size_t want = need;
            if (slack > 0 && want <= (std::numeric_limits<size_t>::max() - slack))
                want += slack;
            v.reserve(want);
        };

        reserve_with_slack(layer.cells);
        reserve_with_slack(layer.fg);
        reserve_with_slack(layer.bg);
        reserve_with_slack(layer.attrs);
        layer.cells.resize(need, U' ');
        layer.fg.resize(need, 0);
        layer.bg.resize(need, 0);
        layer.attrs.resize(need, 0);
    }

    // Row growth should always be reflected in SAUCE (screen height hint).
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
}

size_t AnsiCanvas::CellIndex(int row, int col) const
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    return static_cast<size_t>(row) * static_cast<size_t>(m_columns) + static_cast<size_t>(col);
}

bool AnsiCanvas::CanvasToLayerLocalForWrite(int layer_index,
                                           int canvas_row,
                                           int canvas_col,
                                           int& out_local_row,
                                           int& out_local_col) const
{
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (m_columns <= 0)
        return false;

    const Layer& layer = m_layers[(size_t)layer_index];
    const long long lr = (long long)canvas_row - (long long)layer.offset_y;
    const long long lc = (long long)canvas_col - (long long)layer.offset_x;
    if (lr < 0 || lc < 0)
        return false;
    if (lc >= (long long)m_columns)
        return false;
    if (lr > (long long)std::numeric_limits<int>::max() || lc > (long long)std::numeric_limits<int>::max())
        return false;
    out_local_row = (int)lr;
    out_local_col = (int)lc;
    return true;
}

bool AnsiCanvas::CanvasToLayerLocalForRead(int layer_index,
                                          int canvas_row,
                                          int canvas_col,
                                          int& out_local_row,
                                          int& out_local_col) const
{
    if (!CanvasToLayerLocalForWrite(layer_index, canvas_row, canvas_col, out_local_row, out_local_col))
        return false;
    if (out_local_row < 0 || out_local_row >= m_rows)
        return false;
    return true;
}

AnsiCanvas::CompositeCell AnsiCanvas::GetCompositeCell(int row, int col) const
{
    CompositeCell out;
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return out;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return out;

    // Background: topmost visible non-zero background wins (space remains "transparent"
    // for glyph compositing, but background can be colored independently).
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
            continue;
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (idx >= layer.bg.size())
            continue;
        const Color32 bg = layer.bg[idx];
        if (bg != 0)
        {
            out.bg = bg;
            break;
        }
    }

    // Glyph + foreground: topmost visible non-space glyph wins. Foreground color and
    // attributes are taken from that same layer.
    //
    // IMPORTANT compositing rule:
    // A space cell does NOT contribute to the foreground plane, even if it has attrs set.
    // This ensures style-only overlays (e.g. underline on spaces) do not occlude glyphs
    // from lower layers, and matches the editor rule: "no glyph => transparent".
    for (int i = (int)m_layers.size() - 1; i >= 0; --i)
    {
        const Layer& layer = m_layers[(size_t)i];
        if (!layer.visible)
            continue;
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForRead(i, row, col, lr, lc))
            continue;
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (idx >= layer.cells.size())
            continue;
        const char32_t cp = layer.cells[idx];
        if (cp == U' ')
            continue;

        out.cp = cp;
        out.fg = (idx < layer.fg.size()) ? layer.fg[idx] : 0;
        out.attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;
        break;
    }

    return out;
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        // Determine old cell value without growing the document unless we will mutate.
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = old_fg;
        const Color32  new_bg = old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_cp == new_cp)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = cp;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

void AnsiCanvas::SetActiveCell(int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = fg;
        const Color32  new_bg = bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
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
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

void AnsiCanvas::ClearActiveCellStyle(int row, int col)
{
    EnsureDocument();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    auto write_one = [&](int write_col)
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(m_active_layer, row, write_col, lr, lc))
            return;

        Layer& layer = m_layers[(size_t)m_active_layer];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = old_cp;
        const Color32  new_fg = 0;
        const Color32  new_bg = 0;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return;
        if (in_bounds && old_fg == 0 && old_bg == 0 && old_attrs == 0)
            return; // no-op

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(m_active_layer, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.fg.size())
            layer.fg[widx] = 0;
        if (widx < layer.bg.size())
            layer.bg[widx] = 0;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
    };

    write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            write_one(mirror_col);
    }
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = old_fg;
        const Color32  new_bg = old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        if (lr >= m_rows)
            EnsureRows(lr + 1);
        const size_t widx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        if (widx < layer.cells.size())
            layer.cells[widx] = new_cp;
        if (widx < layer.attrs.size())
            layer.attrs[widx] = new_attrs;
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp, Color32 fg, Color32 bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = fg;
        const Color32  new_bg = bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
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
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::SetLayerCell(int layer_index, int row, int col, char32_t cp, Color32 fg, Color32 bg, Attrs attrs)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;

        Layer& layer = m_layers[(size_t)layer_index];
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = fg;
        const Color32  new_bg = bg;
        const Attrs    new_attrs = attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
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
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

char32_t AnsiCanvas::GetLayerCell(int layer_index, int row, int col) const
{
    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return U' ';
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return U' ';
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return U' ';

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return U' ';
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.cells.size())
        return U' ';
    return layer.cells[idx];
}

bool AnsiCanvas::GetLayerCellColors(int layer_index, int row, int col, Color32& out_fg, Color32& out_bg) const
{
    out_fg = 0;
    out_bg = 0;

    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return false;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return false;

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.fg.size() || idx >= layer.bg.size())
        return false;
    out_fg = layer.fg[idx];
    out_bg = layer.bg[idx];
    return true;
}

bool AnsiCanvas::GetLayerCellAttrs(int layer_index, int row, int col, Attrs& out_attrs) const
{
    out_attrs = 0;

    if (m_columns <= 0 || m_rows <= 0 || m_layers.empty())
        return false;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_columns)
        return false;

    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForRead(layer_index, row, col, lr, lc))
        return false;
    const Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
    if (idx >= layer.attrs.size())
        return false;
    out_attrs = layer.attrs[idx];
    return true;
}

void AnsiCanvas::ClearLayerCellStyleInternal(int layer_index, int row, int col)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;
    int lr = 0, lc = 0;
    if (!CanvasToLayerLocalForWrite(layer_index, row, col, lr, lc))
        return;
    Layer& layer = m_layers[(size_t)layer_index];
    const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;

    const bool in_bounds = (lr < m_rows);
    const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
    const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
    const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
    const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

    const char32_t new_cp = old_cp;
    const Color32  new_fg = 0;
    const Color32  new_bg = 0;
    const Attrs    new_attrs = 0;

    if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                      old_cp, old_fg, old_bg, old_attrs,
                                      new_cp, new_fg, new_bg, new_attrs))
        return;
    if (in_bounds && old_fg == 0 && old_bg == 0 && old_attrs == 0)
        return;

    EnsureRows(lr + 1);
    if (idx < layer.fg.size())
        layer.fg[idx] = 0;
    if (idx < layer.bg.size())
        layer.bg[idx] = 0;
    if (idx < layer.attrs.size())
        layer.attrs[idx] = new_attrs;
}

bool AnsiCanvas::ClearLayerCellStyle(int layer_index, int row, int col)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return false;
    // Avoid capturing an undo snapshot if the mutation is blocked by alpha-lock or is a no-op.
    Layer& layer = m_layers[(size_t)layer_index];
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= m_columns) col = m_columns - 1;

    auto write_one = [&](int write_col) -> bool
    {
        int lr = 0, lc = 0;
        if (!CanvasToLayerLocalForWrite(layer_index, row, write_col, lr, lc))
            return false;
        const bool in_bounds = (lr < m_rows);
        const size_t idx = (size_t)lr * (size_t)m_columns + (size_t)lc;
        const char32_t old_cp = (in_bounds && idx < layer.cells.size()) ? layer.cells[idx] : U' ';
        const Color32  old_fg = (in_bounds && idx < layer.fg.size())    ? layer.fg[idx]    : 0;
        const Color32  old_bg = (in_bounds && idx < layer.bg.size())    ? layer.bg[idx]    : 0;
        const Attrs    old_attrs = (in_bounds && idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = old_cp;
        const Color32  new_fg = 0;
        const Color32  new_bg = 0;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            return false;
        if (in_bounds && old_fg == 0 && old_bg == 0 && old_attrs == 0)
            return true;

        PrepareUndoForMutation();
        EnsureUndoCaptureIsPatch();
        CaptureUndoPageIfNeeded(layer_index, lr);
        ClearLayerCellStyleInternal(layer_index, row, write_col);
        return true;
    };

    const bool ok_primary = write_one(col);

    const bool mirror = m_mirror_mode && m_tool_running && m_columns > 1;
    if (mirror)
    {
        const int mirror_col = (m_columns - 1) - col;
        if (mirror_col != col)
            (void)write_one(mirror_col);
    }

    return ok_primary;
}

bool AnsiCanvas::ClearLayer(int layer_index, char32_t cp)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
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

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const char32_t old_cp = layer.cells[idx];
        const Color32  old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : 0;
        const Color32  old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : 0;
        const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp;
        const Color32  new_fg = 0;
        const Color32  new_bg = 0;
        const Attrs    new_attrs = 0;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg && old_attrs == new_attrs)
            continue;

        prepare();
        if (m_columns > 0)
            CaptureUndoPageIfNeeded(layer_index, (int)(idx / (size_t)m_columns));
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        if (idx < layer.attrs.size()) layer.attrs[idx] = new_attrs;
        did_anything = true;
    }
    return did_anything;
}

bool AnsiCanvas::FillLayer(int layer_index,
                           std::optional<char32_t> cp,
                           std::optional<Color32> fg,
                           std::optional<Color32> bg)
{
    EnsureDocument();
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
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

    const size_t n = layer.cells.size();
    for (size_t idx = 0; idx < n; ++idx)
    {
        const char32_t old_cp = layer.cells[idx];
        const Color32  old_fg = (idx < layer.fg.size()) ? layer.fg[idx] : 0;
        const Color32  old_bg = (idx < layer.bg.size()) ? layer.bg[idx] : 0;
        const Attrs    old_attrs = (idx < layer.attrs.size()) ? layer.attrs[idx] : 0;

        const char32_t new_cp = cp.has_value() ? *cp : old_cp;
        const Color32  new_fg = fg.has_value() ? *fg : old_fg;
        const Color32  new_bg = bg.has_value() ? *bg : old_bg;
        const Attrs    new_attrs = old_attrs;

        if (!TransparencyTransitionAllowed(layer.lock_transparency,
                                          old_cp, old_fg, old_bg, old_attrs,
                                          new_cp, new_fg, new_bg, new_attrs))
            continue;
        if (old_cp == new_cp && old_fg == new_fg && old_bg == new_bg)
            continue;

        prepare();
        if (m_columns > 0)
            CaptureUndoPageIfNeeded(layer_index, (int)(idx / (size_t)m_columns));
        if (idx < layer.cells.size()) layer.cells[idx] = new_cp;
        if (idx < layer.fg.size())    layer.fg[idx]    = new_fg;
        if (idx < layer.bg.size())    layer.bg[idx]    = new_bg;
        did_anything = true;
    }
    return did_anything;
}

// ---- end inlined from canvas_layers.inc ----


