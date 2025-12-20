// Layer manager UI component for utf8-art-editor.
// Keeps layer GUI logic out of AnsiCanvas so the canvas stays focused on:
//  - data model (grid + layers)
//  - rendering the grid
//  - editing primitives
//
// The LayerManager window targets the app's current "active" canvas (decided by the main loop).

#pragma once

#include <vector>

class AnsiCanvas;
struct SessionState;

struct LayerManagerCanvasRef
{
    int        id     = 0;
    AnsiCanvas* canvas = nullptr;
};

class LayerManager
{
public:
    void Render(const char* title,
                bool* p_open,
                AnsiCanvas* active_canvas,
                SessionState* session = nullptr,
                bool apply_placement_this_frame = false);

private:
    char rename_buf_[256] = {0};

    // Rename popup state (kept so the modal can remain open across frames).
    AnsiCanvas*  rename_target_canvas_ = nullptr;
    int          rename_target_layer_index_ = -1;
    int          rename_popup_serial_ = 0;
    int          rename_popup_active_serial_ = 0;
    bool         rename_popup_requested_open_ = false;

    // Inline rename state (double-click name to edit, Enter/blur to commit).
    AnsiCanvas* inline_rename_canvas_ = nullptr;
    int         inline_rename_layer_index_ = -1;
    char        inline_rename_buf_[256] = {0};
    bool        inline_rename_request_focus_ = false;
};


