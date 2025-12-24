#pragma once

#include <string>
#include <vector>

#include "core/canvas.h"

class AnsiCanvas;
struct SessionState;

// Brush Palette window:
// - In-memory gallery of multi-cell brushes ("stamps")
// - Can capture a brush from the active canvas selection
// - Can apply a brush as the active canvas's current brush
class BrushPaletteWindow
{
public:
    struct Entry
    {
        std::string name;
        AnsiCanvas::Brush brush;
    };

    BrushPaletteWindow();

    // If true, the user interacted in a way that implies they want to stamp a brush
    // (e.g. selected/created a brush). Callers can use this as a UX hint to auto-switch
    // to the Brush tool.
    bool TakeActivateBrushToolRequested()
    {
        const bool v = request_activate_brush_tool_;
        request_activate_brush_tool_ = false;
        return v;
    }

    // Returns true if the window is open after rendering.
    bool Render(const char* window_title,
                bool* p_open = nullptr,
                AnsiCanvas* active_canvas = nullptr,
                SessionState* session = nullptr,
                bool apply_placement_this_frame = false);

private:
    void RenderTopBar(AnsiCanvas* active_canvas, SessionState* session);
    void RenderSettingsContents();
    void RenderGrid(AnsiCanvas* active_canvas, SessionState* session);

    void EnsureLoaded(AnsiCanvas* active_canvas, SessionState* session);
    bool LoadFromFile(const char* path, AnsiCanvas* active_canvas, std::string& error);
    bool SaveToFile(const char* path, std::string& error) const;

    // One-time migration path (session.json -> brush-palettes.json) for older installs.
    void LoadFromSessionBrushPalette(SessionState* session, AnsiCanvas* active_canvas);

private:
    std::vector<Entry> entries_;
    int selected_ = -1;
    AnsiCanvas* last_active_canvas_ = nullptr;

    // UI state
    bool capture_composite_ = true;
    int thumb_px_ = 72;
    char new_name_buf_[128] = {};
    bool settings_open_ = false;

    // Inline rename UI state (double-click label to rename).
    int inline_rename_index_ = -1;
    char inline_rename_buf_[128] = {};
    bool inline_rename_request_focus_ = false;

    // File persistence state
    bool        loaded_ = false;
    std::string file_path_;
    std::string last_error_;
    bool        request_save_ = false;
    bool        request_reload_ = false;
    bool        migrated_from_session_ = false;

    // UX hint for host: user selected/created a brush and likely wants the Brush tool active.
    bool        request_activate_brush_tool_ = false;
};


