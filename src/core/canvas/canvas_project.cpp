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

// ---- inlined from canvas_project.inc ----
AnsiCanvas::ProjectState AnsiCanvas::GetProjectState() const
{
    auto to_project_layer = [&](const Layer& l) -> ProjectLayer
    {
        ProjectLayer out;
        out.name = l.name;
        out.visible = l.visible;
        out.lock_transparency = l.lock_transparency;
        out.offset_x = l.offset_x;
        out.offset_y = l.offset_y;
        out.cells = l.cells;
        out.fg = l.fg;
        out.bg = l.bg;
        return out;
    };

    auto to_project_snapshot = [&](const Snapshot& s) -> ProjectSnapshot
    {
        ProjectSnapshot out;
        out.columns = s.columns;
        out.rows = s.rows;
        out.active_layer = s.active_layer;
        out.caret_row = s.caret_row;
        out.caret_col = s.caret_col;
        out.layers.reserve(s.layers.size());
        for (const auto& l : s.layers)
            out.layers.push_back(to_project_layer(l));
        return out;
    };
    auto to_project_undo_entry = [&](const UndoEntry& e) -> ProjectState::ProjectUndoEntry
    {
        ProjectState::ProjectUndoEntry out;
        if (e.kind == UndoEntry::Kind::Patch)
        {
            out.kind = ProjectState::ProjectUndoEntry::Kind::Patch;
            out.patch.columns = e.patch.columns;
            out.patch.rows = e.patch.rows;
            out.patch.active_layer = e.patch.active_layer;
            out.patch.caret_row = e.patch.caret_row;
            out.patch.caret_col = e.patch.caret_col;
            out.patch.state_token = e.patch.state_token;
            out.patch.page_rows = e.patch.page_rows;
            out.patch.layers.clear();
            out.patch.layers.reserve(e.patch.layers.size());
            for (const auto& lm : e.patch.layers)
            {
                ProjectState::ProjectUndoEntry::PatchLayerMeta plm;
                plm.name = lm.name;
                plm.visible = lm.visible;
                plm.lock_transparency = lm.lock_transparency;
                plm.offset_x = lm.offset_x;
                plm.offset_y = lm.offset_y;
                out.patch.layers.push_back(std::move(plm));
            }
            out.patch.pages.clear();
            out.patch.pages.reserve(e.patch.pages.size());
            for (const auto& pg : e.patch.pages)
            {
                ProjectState::ProjectUndoEntry::PatchPage p;
                p.layer = pg.layer;
                p.page = pg.page;
                p.page_rows = pg.page_rows;
                p.row_count = pg.row_count;
                p.cells = pg.cells;
                p.fg = pg.fg;
                p.bg = pg.bg;
                out.patch.pages.push_back(std::move(p));
            }
        }
        else
        {
            out.kind = ProjectState::ProjectUndoEntry::Kind::Snapshot;
            out.snapshot = to_project_snapshot(e.snapshot);
        }
        return out;
    };

    ProjectState out;
    out.version = 5;
    out.colour_palette_title = m_colour_palette_title;
    out.sauce = m_sauce;
    out.current = to_project_snapshot(MakeSnapshot());
    out.undo_limit = m_undo_limit;

    out.undo.reserve(m_undo_stack.size());
    for (const auto& s : m_undo_stack)
        out.undo.push_back(to_project_undo_entry(s));
    out.redo.reserve(m_redo_stack.size());
    for (const auto& s : m_redo_stack)
        out.redo.push_back(to_project_undo_entry(s));
    return out;
}

bool AnsiCanvas::SetProjectState(const ProjectState& state, std::string& out_error)
{
    out_error.clear();

    auto to_internal_layer = [&](const ProjectLayer& l, Layer& out, std::string& err) -> bool
    {
        out.name = l.name;
        out.visible = l.visible;
        out.lock_transparency = l.lock_transparency;
        out.offset_x = l.offset_x;
        out.offset_y = l.offset_y;
        out.cells = l.cells;
        out.fg = l.fg;
        out.bg = l.bg;

        if (!out.fg.empty() && out.fg.size() != out.cells.size())
        {
            err = "Layer fg size does not match cells size.";
            return false;
        }
        if (!out.bg.empty() && out.bg.size() != out.cells.size())
        {
            err = "Layer bg size does not match cells size.";
            return false;
        }

        if (out.fg.empty())
            out.fg.assign(out.cells.size(), 0);
        if (out.bg.empty())
            out.bg.assign(out.cells.size(), 0);
        return true;
    };

    auto to_internal_snapshot = [&](const ProjectSnapshot& s, Snapshot& out, std::string& err) -> bool
    {
        out.columns = (s.columns > 0) ? s.columns : 80;
        if (out.columns > 4096) out.columns = 4096;
        out.rows = (s.rows > 0) ? s.rows : 1;
        out.active_layer = s.active_layer;
        out.caret_row = s.caret_row;
        out.caret_col = s.caret_col;
        out.layers.clear();
        out.layers.reserve(s.layers.size());
        for (const auto& pl : s.layers)
        {
            Layer l;
            if (!to_internal_layer(pl, l, err))
                return false;
            out.layers.push_back(std::move(l));
        }
        return true;
    };

    auto to_internal_undo_entry = [&](const ProjectState::ProjectUndoEntry& e, UndoEntry& out, std::string& err) -> bool
    {
        (void)err;
        out = UndoEntry{};
        if (e.kind == ProjectState::ProjectUndoEntry::Kind::Patch)
        {
            out.kind = UndoEntry::Kind::Patch;
            out.patch.columns = e.patch.columns;
            out.patch.rows = e.patch.rows;
            out.patch.active_layer = e.patch.active_layer;
            out.patch.caret_row = e.patch.caret_row;
            out.patch.caret_col = e.patch.caret_col;
            out.patch.state_token = e.patch.state_token;
            out.patch.page_rows = e.patch.page_rows;
            out.patch.layers.clear();
            out.patch.layers.reserve(e.patch.layers.size());
            for (const auto& lm : e.patch.layers)
            {
                UndoEntry::PatchLayerMeta ilm;
                ilm.name = lm.name;
                ilm.visible = lm.visible;
                ilm.lock_transparency = lm.lock_transparency;
                ilm.offset_x = lm.offset_x;
                ilm.offset_y = lm.offset_y;
                out.patch.layers.push_back(std::move(ilm));
            }
            out.patch.pages.clear();
            out.patch.pages.reserve(e.patch.pages.size());
            for (const auto& pg : e.patch.pages)
            {
                UndoEntry::PatchPage ip;
                ip.layer = pg.layer;
                ip.page = pg.page;
                ip.page_rows = pg.page_rows;
                ip.row_count = pg.row_count;
                ip.cells = pg.cells;
                ip.fg = pg.fg;
                ip.bg = pg.bg;
                out.patch.pages.push_back(std::move(ip));
            }
            return true;
        }
        out.kind = UndoEntry::Kind::Snapshot;
        return to_internal_snapshot(e.snapshot, out.snapshot, err);
    };

    // Convert everything up-front so we can fail without mutating `this`.
    Snapshot current_internal;
    if (!to_internal_snapshot(state.current, current_internal, out_error))
        return false;

    std::vector<UndoEntry> undo_internal;
    undo_internal.reserve(state.undo.size());
    for (const auto& s : state.undo)
    {
        UndoEntry tmp;
        if (!to_internal_undo_entry(s, tmp, out_error))
            return false;
        undo_internal.push_back(std::move(tmp));
    }

    std::vector<UndoEntry> redo_internal;
    redo_internal.reserve(state.redo.size());
    for (const auto& s : state.redo)
    {
        UndoEntry tmp;
        if (!to_internal_undo_entry(s, tmp, out_error))
            return false;
        redo_internal.push_back(std::move(tmp));
    }

    // Assign runtime-only state tokens so undo/redo can restore the "dirty" marker correctly.
    std::uint64_t next_token = 1;
    auto bump = [&]() {
        const std::uint64_t v = next_token ? next_token : 1;
        ++next_token;
        if (next_token == 0)
            ++next_token;
        return v;
    };
    // For snapshot entries, we can assign snapshot.state_token; for patch entries, assign patch.state_token.
    for (auto& e : undo_internal)
    {
        const std::uint64_t t = bump();
        if (e.kind == UndoEntry::Kind::Snapshot) e.snapshot.state_token = t;
        else e.patch.state_token = t;
    }
    for (auto& e : redo_internal)
    {
        const std::uint64_t t = bump();
        if (e.kind == UndoEntry::Kind::Snapshot) e.snapshot.state_token = t;
        else e.patch.state_token = t;
    }
    current_internal.state_token = bump();

    // Apply in one go.
    m_has_focus = false;
    m_typed_queue.clear();
    m_key_events = KeyEvents{};
    m_mouse_capture = false;
    m_cursor_valid = false;

    m_undo_capture_active = false;
    m_undo_capture_modified = false;
    m_undo_capture_has_entry = false;
    m_undo_applying_snapshot = false;
    m_undo_capture_entry.reset();
    m_undo_capture_page_index.clear();

    // 0 = unlimited.
    m_undo_limit = state.undo_limit;
    m_undo_stack = std::move(undo_internal);
    m_redo_stack = std::move(redo_internal);
    SetUndoLimit(m_undo_limit);

    // Metadata (non-undoable, persisted).
    m_sauce = state.sauce;
    m_colour_palette_title = state.colour_palette_title;

    ApplySnapshot(current_internal);

    // Clamp active layer and ensure we have at least one layer even for malformed saves.
    EnsureDocument();

    // Post-load: ensure SAUCE defaults and geometry are consistent with the applied snapshot.
    EnsureSauceDefaultsAndSyncGeometry(m_sauce, m_columns, m_rows);
    return true;
}

// ---- end inlined from canvas_project.inc ----


