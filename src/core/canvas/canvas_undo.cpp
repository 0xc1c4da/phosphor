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

// ---- inlined from canvas_undo.inc ----
// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------

AnsiCanvas::Snapshot AnsiCanvas::MakeSnapshot() const
{
    Snapshot s;
    s.columns = m_columns;
    s.rows = m_rows;
    s.active_layer = m_active_layer;
    s.caret_row = m_caret_row;
    s.caret_col = m_caret_col;
    s.layers = m_layers;
    s.state_token = m_state_token;
    return s;
}

void AnsiCanvas::ApplySnapshot(const Snapshot& s)
{
    m_undo_applying_snapshot = true;

    m_columns = (s.columns > 0) ? s.columns : 80;
    if (m_columns > 4096) m_columns = 4096;
    m_rows    = (s.rows > 0) ? s.rows : 1;
    m_layers  = s.layers;
    m_active_layer = s.active_layer;
    m_caret_row = s.caret_row;
    m_caret_col = s.caret_col;
    m_state_token = s.state_token ? s.state_token : 1;

    // Transient interaction state; recomputed next frame.
    m_cursor_valid = false;
    m_mouse_capture = false;

    // Re-establish invariants.
    EnsureDocument();
    if (m_rows <= 0) m_rows = 1;
    EnsureRows(m_rows);
    if (m_caret_row < 0) m_caret_row = 0;
    if (m_caret_col < 0) m_caret_col = 0;
    if (m_caret_col >= m_columns) m_caret_col = m_columns - 1;

    m_undo_applying_snapshot = false;

    // Keep SAUCE geometry in sync with the restored document.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);

    // Snapshot application always changes visible content (Undo/Redo/load).
    TouchContent();
}

void AnsiCanvas::BeginUndoCapture()
{
    if (m_undo_applying_snapshot)
        return;
    m_undo_capture_active = true;
    m_undo_capture_modified = false;
    m_undo_capture_has_entry = false;
    m_undo_capture_entry.reset();
    m_undo_capture_page_index.clear();
}

void AnsiCanvas::EndUndoCapture()
{
    if (!m_undo_capture_active)
        return;

    if (m_undo_capture_modified && m_undo_capture_has_entry && m_undo_capture_entry.has_value())
    {
        m_undo_stack.push_back(std::move(*m_undo_capture_entry));
        if (m_undo_limit > 0 && m_undo_stack.size() > m_undo_limit)
            m_undo_stack.erase(m_undo_stack.begin(),
                               m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));
        m_redo_stack.clear();
    }

    m_undo_capture_active = false;
    m_undo_capture_modified = false;
    m_undo_capture_has_entry = false;
    m_undo_capture_entry.reset();
    m_undo_capture_page_index.clear();
}

void AnsiCanvas::PrepareUndoForMutation()
{
    if (m_undo_applying_snapshot)
        return;

    // Many callers mutate canvas content from outside AnsiCanvas::Render() (e.g. ANSL scripts).
    // Those mutations still need to bump the content revision so dependent UI caches (minimap
    // texture, previews) update immediately, even if we're not currently capturing an undo step.
    //
    // Performance: if an ExternalMutationScope is active AND we are not capturing undo,
    // coalesce state/content bumps to at most once per scope.
    if (!m_undo_capture_active && m_external_mutation_depth > 0)
    {
        if (!m_external_mutation_bumped)
        {
            // Any mutation to project content changes the document state token.
            // Avoid wrap to 0 (treat 0 as "uninitialized" in some contexts).
            ++m_state_token;
            if (m_state_token == 0)
                ++m_state_token;
            TouchContent();
            m_external_mutation_bumped = true;
        }
        return;
    }

    // Any mutation to project content changes the document state token.
    // Avoid wrap to 0 (treat 0 as "uninitialized" in some contexts).
    ++m_state_token;
    if (m_state_token == 0)
        ++m_state_token;
    if (!m_undo_capture_active)
    {
        TouchContent();
        return;
    }

    m_undo_capture_modified = true;

    // Content is changing within this capture scope.
    TouchContent();
}

void AnsiCanvas::EnsureUndoCaptureIsPatch()
{
    if (!m_undo_capture_active)
        return;
    if (m_undo_capture_has_entry && m_undo_capture_entry.has_value())
    {
        if (m_undo_capture_entry->kind == UndoEntry::Kind::Patch)
            return;
        // Already snapshot; keep it.
        return;
    }

    UndoEntry e;
    e.kind = UndoEntry::Kind::Patch;
    e.patch.columns = m_columns;
    e.patch.rows = m_rows;
    e.patch.active_layer = m_active_layer;
    e.patch.caret_row = m_caret_row;
    e.patch.caret_col = m_caret_col;
    e.patch.state_token = m_state_token;
    e.patch.page_rows = 64;
    e.patch.layers.clear();
    e.patch.layers.reserve(m_layers.size());
    for (const Layer& l : m_layers)
    {
        UndoEntry::PatchLayerMeta lm;
        lm.name = l.name;
        lm.visible = l.visible;
        lm.lock_transparency = l.lock_transparency;
        lm.offset_x = l.offset_x;
        lm.offset_y = l.offset_y;
        e.patch.layers.push_back(std::move(lm));
    }
    e.patch.pages.clear();

    m_undo_capture_entry = std::move(e);
    m_undo_capture_has_entry = true;
    m_undo_capture_page_index.clear();
}

void AnsiCanvas::EnsureUndoCaptureIsSnapshot()
{
    if (!m_undo_capture_active)
        return;
    if (m_undo_capture_has_entry && m_undo_capture_entry.has_value())
    {
        if (m_undo_capture_entry->kind == UndoEntry::Kind::Snapshot)
            return;
        // If we've already started capturing deltas, we cannot safely "promote" without
        // reconstructing full previous state. For now, keep the patch entry.
        return;
    }
    UndoEntry e;
    e.kind = UndoEntry::Kind::Snapshot;
    e.snapshot = MakeSnapshot();
    m_undo_capture_entry = std::move(e);
    m_undo_capture_has_entry = true;
    m_undo_capture_page_index.clear();
}

void AnsiCanvas::CaptureUndoPageIfNeeded(int layer_index, int row)
{
    if (!m_undo_capture_active)
        return;
    if (!m_undo_capture_has_entry)
        EnsureUndoCaptureIsPatch();
    if (!m_undo_capture_entry.has_value())
        return;
    if (m_undo_capture_entry->kind != UndoEntry::Kind::Patch)
        return;
    if (layer_index < 0 || layer_index >= (int)m_layers.size())
        return;

    UndoEntry::Patch& p = m_undo_capture_entry->patch;
    const int page_rows = (p.page_rows > 0) ? p.page_rows : 64;
    if (row < 0)
        row = 0;
    const int page = row / page_rows;
    const std::uint64_t key = (std::uint64_t)((std::uint32_t)layer_index) << 32 | (std::uint32_t)page;
    auto it = m_undo_capture_page_index.find(key);
    if (it != m_undo_capture_page_index.end())
        return;

    // Only capture rows that existed at the start of the capture.
    const int start_row = page * page_rows;
    if (start_row >= p.rows)
    {
        // This page is entirely beyond the old document height; undo will shrink rows anyway.
        m_undo_capture_page_index[key] = (size_t)-1;
        return;
    }
    const int row_count = std::min(page_rows, p.rows - start_row);
    if (row_count <= 0)
    {
        m_undo_capture_page_index[key] = (size_t)-1;
        return;
    }

    const int cols = p.columns;
    const size_t n = (size_t)row_count * (size_t)std::max(0, cols);
    UndoEntry::PatchPage page_data;
    page_data.layer = layer_index;
    page_data.page = page;
    page_data.page_rows = page_rows;
    page_data.row_count = row_count;
    page_data.cells.assign(n, U' ');
    page_data.fg.assign(n, kUnsetIndex16);
    page_data.bg.assign(n, kUnsetIndex16);
    page_data.attrs.assign(n, 0);

    const Layer& layer = m_layers[(size_t)layer_index];
    for (int r = 0; r < row_count; ++r)
    {
        const int rr = start_row + r;
        const size_t base = (size_t)r * (size_t)cols;
        const size_t idx0 = (size_t)rr * (size_t)cols;
        for (int c = 0; c < cols; ++c)
        {
            const size_t idx = idx0 + (size_t)c;
            const size_t out = base + (size_t)c;
            if (idx < layer.cells.size()) page_data.cells[out] = layer.cells[idx];
            if (idx < layer.fg.size())    page_data.fg[out]    = layer.fg[idx];
            if (idx < layer.bg.size())    page_data.bg[out]    = layer.bg[idx];
            if (idx < layer.attrs.size()) page_data.attrs[out] = layer.attrs[idx];
        }
    }

    const size_t new_index = p.pages.size();
    p.pages.push_back(std::move(page_data));
    m_undo_capture_page_index[key] = new_index;
}

bool AnsiCanvas::CanUndo() const
{
    return !m_undo_stack.empty();
}

bool AnsiCanvas::CanRedo() const
{
    return !m_redo_stack.empty();
}

bool AnsiCanvas::Undo()
{
    if (m_undo_stack.empty())
        return false;
    if (m_undo_applying_snapshot)
        return false;

    UndoEntry prev = std::move(m_undo_stack.back());
    m_undo_stack.pop_back();

    // Capture current state for redo, matching the granularity of the undo entry.
    UndoEntry cur;
    cur.kind = prev.kind;
    if (prev.kind == UndoEntry::Kind::Snapshot)
    {
        cur.snapshot = MakeSnapshot();
    }
    else
    {
        cur.kind = UndoEntry::Kind::Patch;
        cur.patch.columns = m_columns;
        cur.patch.rows = m_rows;
        cur.patch.active_layer = m_active_layer;
        cur.patch.caret_row = m_caret_row;
        cur.patch.caret_col = m_caret_col;
        cur.patch.state_token = m_state_token;
        cur.patch.page_rows = prev.patch.page_rows;
        cur.patch.layers.clear();
        cur.patch.layers.reserve(m_layers.size());
        for (const Layer& l : m_layers)
        {
            UndoEntry::PatchLayerMeta lm;
            lm.name = l.name;
            lm.visible = l.visible;
            lm.lock_transparency = l.lock_transparency;
            lm.offset_x = l.offset_x;
            lm.offset_y = l.offset_y;
            cur.patch.layers.push_back(std::move(lm));
        }
        cur.patch.pages.clear();
        cur.patch.pages.reserve(prev.patch.pages.size());
        for (const auto& pg : prev.patch.pages)
        {
            UndoEntry::PatchPage outp;
            outp.layer = pg.layer;
            outp.page = pg.page;
            outp.page_rows = pg.page_rows;
            outp.row_count = pg.row_count;
            const int cols = cur.patch.columns;
            const int start_row = outp.page * outp.page_rows;
            const int row_count = std::min(outp.row_count, std::max(0, cur.patch.rows - start_row));
            outp.row_count = std::max(0, row_count);
            const size_t n = (size_t)outp.row_count * (size_t)std::max(0, cols);
            outp.cells.assign(n, U' ');
            outp.fg.assign(n, kUnsetIndex16);
            outp.bg.assign(n, kUnsetIndex16);
            outp.attrs.assign(n, 0);
            if (outp.layer >= 0 && outp.layer < (int)m_layers.size())
            {
                const Layer& layer = m_layers[(size_t)outp.layer];
                for (int r = 0; r < outp.row_count; ++r)
                {
                    const int rr = start_row + r;
                    const size_t base = (size_t)r * (size_t)cols;
                    const size_t idx0 = (size_t)rr * (size_t)cols;
                    for (int c = 0; c < cols; ++c)
                    {
                        const size_t idx = idx0 + (size_t)c;
                        const size_t outi = base + (size_t)c;
                        if (idx < layer.cells.size()) outp.cells[outi] = layer.cells[idx];
                        if (idx < layer.fg.size())    outp.fg[outi]    = layer.fg[idx];
                        if (idx < layer.bg.size())    outp.bg[outi]    = layer.bg[idx];
                        if (idx < layer.attrs.size()) outp.attrs[outi] = layer.attrs[idx];
                    }
                }
            }
            cur.patch.pages.push_back(std::move(outp));
        }
    }

    // Apply the undo entry.
    if (prev.kind == UndoEntry::Kind::Snapshot)
    {
        ApplySnapshot(prev.snapshot);
    }
    else
    {
        m_undo_applying_snapshot = true;
        // Restore metadata.
        m_columns = prev.patch.columns > 0 ? prev.patch.columns : m_columns;
        if (m_columns > 4096) m_columns = 4096;
        m_rows = prev.patch.rows > 0 ? prev.patch.rows : 1;
        m_active_layer = prev.patch.active_layer;
        m_caret_row = prev.patch.caret_row;
        m_caret_col = prev.patch.caret_col;
        m_state_token = prev.patch.state_token ? prev.patch.state_token : 1;

        // Restore layer metadata and ensure layer count.
        if ((int)m_layers.size() != (int)prev.patch.layers.size())
            m_layers.resize(prev.patch.layers.size());
        for (size_t i = 0; i < prev.patch.layers.size(); ++i)
        {
            m_layers[i].name = prev.patch.layers[i].name;
            m_layers[i].visible = prev.patch.layers[i].visible;
            m_layers[i].lock_transparency = prev.patch.layers[i].lock_transparency;
            m_layers[i].offset_x = prev.patch.layers[i].offset_x;
            m_layers[i].offset_y = prev.patch.layers[i].offset_y;
        }

        EnsureDocument();
        if (m_rows <= 0) m_rows = 1;
        EnsureRows(m_rows);

        // Restore captured pages.
        const int cols = m_columns;
        for (const auto& pg : prev.patch.pages)
        {
            if (pg.layer < 0 || pg.layer >= (int)m_layers.size())
                continue;
            const int start_row = pg.page * pg.page_rows;
            const int row_count = pg.row_count;
            if (row_count <= 0 || cols <= 0)
                continue;
            const size_t expected = (size_t)row_count * (size_t)cols;
            if (pg.cells.size() != expected || pg.fg.size() != expected || pg.bg.size() != expected || pg.attrs.size() != expected)
                continue;
            if (start_row >= m_rows)
                continue;
            const int max_rows = std::min(row_count, m_rows - start_row);
            Layer& layer = m_layers[(size_t)pg.layer];
            for (int r = 0; r < max_rows; ++r)
            {
                const int rr = start_row + r;
                const size_t base = (size_t)r * (size_t)cols;
                const size_t idx0 = (size_t)rr * (size_t)cols;
                for (int c = 0; c < cols; ++c)
                {
                    const size_t idx = idx0 + (size_t)c;
                    const size_t src = base + (size_t)c;
                    if (idx < layer.cells.size()) layer.cells[idx] = pg.cells[src];
                    if (idx < layer.fg.size())    layer.fg[idx]    = pg.fg[src];
                    if (idx < layer.bg.size())    layer.bg[idx]    = pg.bg[src];
                    if (idx < layer.attrs.size()) layer.attrs[idx] = pg.attrs[src];
                }
            }
        }

        // Transient interaction state; recomputed next frame.
        m_cursor_valid = false;
        m_mouse_capture = false;
        m_undo_applying_snapshot = false;

        EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
        TouchContent();
    }

    m_redo_stack.push_back(std::move(cur));
    return true;
}

bool AnsiCanvas::Redo()
{
    if (m_redo_stack.empty())
        return false;
    if (m_undo_applying_snapshot)
        return false;

    UndoEntry next = std::move(m_redo_stack.back());
    m_redo_stack.pop_back();

    // Capture current state for undo, matching the granularity of the redo entry.
    UndoEntry cur;
    cur.kind = next.kind;
    if (next.kind == UndoEntry::Kind::Snapshot)
    {
        cur.snapshot = MakeSnapshot();
    }
    else
    {
        cur.kind = UndoEntry::Kind::Patch;
        cur.patch.columns = m_columns;
        cur.patch.rows = m_rows;
        cur.patch.active_layer = m_active_layer;
        cur.patch.caret_row = m_caret_row;
        cur.patch.caret_col = m_caret_col;
        cur.patch.state_token = m_state_token;
        cur.patch.page_rows = next.patch.page_rows;
        cur.patch.layers.clear();
        cur.patch.layers.reserve(m_layers.size());
        for (const Layer& l : m_layers)
        {
            UndoEntry::PatchLayerMeta lm;
            lm.name = l.name;
            lm.visible = l.visible;
            lm.lock_transparency = l.lock_transparency;
            lm.offset_x = l.offset_x;
            lm.offset_y = l.offset_y;
            cur.patch.layers.push_back(std::move(lm));
        }
        cur.patch.pages.clear();
        cur.patch.pages.reserve(next.patch.pages.size());
        for (const auto& pg : next.patch.pages)
        {
            UndoEntry::PatchPage outp;
            outp.layer = pg.layer;
            outp.page = pg.page;
            outp.page_rows = pg.page_rows;
            outp.row_count = pg.row_count;
            const int cols = cur.patch.columns;
            const int start_row = outp.page * outp.page_rows;
            const int row_count = std::min(outp.row_count, std::max(0, cur.patch.rows - start_row));
            outp.row_count = std::max(0, row_count);
            const size_t n = (size_t)outp.row_count * (size_t)std::max(0, cols);
            outp.cells.assign(n, U' ');
            outp.fg.assign(n, kUnsetIndex16);
            outp.bg.assign(n, kUnsetIndex16);
            outp.attrs.assign(n, 0);
            if (outp.layer >= 0 && outp.layer < (int)m_layers.size())
            {
                const Layer& layer = m_layers[(size_t)outp.layer];
                for (int r = 0; r < outp.row_count; ++r)
                {
                    const int rr = start_row + r;
                    const size_t base = (size_t)r * (size_t)cols;
                    const size_t idx0 = (size_t)rr * (size_t)cols;
                    for (int c = 0; c < cols; ++c)
                    {
                        const size_t idx = idx0 + (size_t)c;
                        const size_t outi = base + (size_t)c;
                        if (idx < layer.cells.size()) outp.cells[outi] = layer.cells[idx];
                        if (idx < layer.fg.size())    outp.fg[outi]    = layer.fg[idx];
                        if (idx < layer.bg.size())    outp.bg[outi]    = layer.bg[idx];
                        if (idx < layer.attrs.size()) outp.attrs[outi] = layer.attrs[idx];
                    }
                }
            }
            cur.patch.pages.push_back(std::move(outp));
        }
    }

    m_undo_stack.push_back(std::move(cur));
    if (m_undo_limit > 0 && m_undo_stack.size() > m_undo_limit)
        m_undo_stack.erase(m_undo_stack.begin(),
                           m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));

    if (next.kind == UndoEntry::Kind::Snapshot)
    {
        ApplySnapshot(next.snapshot);
    }
    else
    {
        // Apply patch (same as in Undo()).
        m_undo_applying_snapshot = true;
        m_columns = next.patch.columns > 0 ? next.patch.columns : m_columns;
        if (m_columns > 4096) m_columns = 4096;
        m_rows = next.patch.rows > 0 ? next.patch.rows : 1;
        m_active_layer = next.patch.active_layer;
        m_caret_row = next.patch.caret_row;
        m_caret_col = next.patch.caret_col;
        m_state_token = next.patch.state_token ? next.patch.state_token : 1;

        if ((int)m_layers.size() != (int)next.patch.layers.size())
            m_layers.resize(next.patch.layers.size());
        for (size_t i = 0; i < next.patch.layers.size(); ++i)
        {
            m_layers[i].name = next.patch.layers[i].name;
            m_layers[i].visible = next.patch.layers[i].visible;
            m_layers[i].lock_transparency = next.patch.layers[i].lock_transparency;
            m_layers[i].offset_x = next.patch.layers[i].offset_x;
            m_layers[i].offset_y = next.patch.layers[i].offset_y;
        }

        EnsureDocument();
        if (m_rows <= 0) m_rows = 1;
        EnsureRows(m_rows);

        const int cols = m_columns;
        for (const auto& pg : next.patch.pages)
        {
            if (pg.layer < 0 || pg.layer >= (int)m_layers.size())
                continue;
            const int start_row = pg.page * pg.page_rows;
            const int row_count = pg.row_count;
            if (row_count <= 0 || cols <= 0)
                continue;
            const size_t expected = (size_t)row_count * (size_t)cols;
            if (pg.cells.size() != expected || pg.fg.size() != expected || pg.bg.size() != expected || pg.attrs.size() != expected)
                continue;
            if (start_row >= m_rows)
                continue;
            const int max_rows = std::min(row_count, m_rows - start_row);
            Layer& layer = m_layers[(size_t)pg.layer];
            for (int r = 0; r < max_rows; ++r)
            {
                const int rr = start_row + r;
                const size_t base = (size_t)r * (size_t)cols;
                const size_t idx0 = (size_t)rr * (size_t)cols;
                for (int c = 0; c < cols; ++c)
                {
                    const size_t idx = idx0 + (size_t)c;
                    const size_t src = base + (size_t)c;
                    if (idx < layer.cells.size()) layer.cells[idx] = pg.cells[src];
                    if (idx < layer.fg.size())    layer.fg[idx]    = pg.fg[src];
                    if (idx < layer.bg.size())    layer.bg[idx]    = pg.bg[src];
                    if (idx < layer.attrs.size()) layer.attrs[idx] = pg.attrs[src];
                }
            }
        }

        m_cursor_valid = false;
        m_mouse_capture = false;
        m_undo_applying_snapshot = false;

        EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
        TouchContent();
    }
    return true;
}

void AnsiCanvas::PushUndoSnapshot()
{
    if (m_undo_applying_snapshot)
        return;

    UndoEntry e;
    e.kind = UndoEntry::Kind::Snapshot;
    e.snapshot = MakeSnapshot();
    m_undo_stack.push_back(std::move(e));
    if (m_undo_limit > 0 && m_undo_stack.size() > m_undo_limit)
        m_undo_stack.erase(m_undo_stack.begin(),
                           m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));
    m_redo_stack.clear();
}

void AnsiCanvas::SetUndoLimit(size_t limit)
{
    m_undo_limit = limit; // 0 = unlimited
    if (m_undo_limit == 0)
        return;

    // Trim oldest entries if needed.
    if (m_undo_stack.size() > m_undo_limit)
        m_undo_stack.erase(m_undo_stack.begin(),
                           m_undo_stack.begin() + (m_undo_stack.size() - m_undo_limit));
    if (m_redo_stack.size() > m_undo_limit)
        m_redo_stack.erase(m_redo_stack.begin(),
                           m_redo_stack.begin() + (m_redo_stack.size() - m_undo_limit));
}

// ---- end inlined from canvas_undo.inc ----


