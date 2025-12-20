#pragma once

#include <string>
#include <vector>

#include "core/canvas.h"
#include "ui/sauce_editor_dialog.h"

// Simple representation of a "canvas" window.
//
// NOTE: This struct used to live in `src/app/main.cpp`. It's kept in `app/` so
// other app-level helpers (session restore, window routing, etc) can share it
// without inflating main further.
struct CanvasWindow
{
    bool open = true;
    int  id   = 0;
    AnsiCanvas canvas;
    SauceEditorDialog sauce_dialog;

    // Session restore: project is loaded lazily from a cached .phos file.
    bool restore_pending = false;
    bool restore_attempted = false;
    std::string restore_phos_cache_rel;
    std::string restore_error;
};

// Shared selection policy for "which canvas does a side-panel operate on?"
// Used by Layer Manager, ANSL Editor, etc.
inline AnsiCanvas* ResolveUiActiveCanvas(std::vector<CanvasWindow>& canvases, int last_active_canvas_id)
{
    // Prefer the last active canvas window id (tracks clicks/focus).
    if (last_active_canvas_id != -1)
    {
        for (auto& c : canvases)
        {
            if (c.open && c.id == last_active_canvas_id)
                return &c.canvas;
        }
    }

    // Fallback: first focused grid.
    for (auto& c : canvases)
    {
        if (c.open && c.canvas.HasFocus())
            return &c.canvas;
    }

    // Fallback: first open canvas.
    for (auto& c : canvases)
    {
        if (c.open)
            return &c.canvas;
    }

    return nullptr;
}


