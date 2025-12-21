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

    // Returns true if the window is open after rendering.
    bool Render(const char* window_title,
                bool* p_open = nullptr,
                AnsiCanvas* active_canvas = nullptr,
                SessionState* session = nullptr,
                bool apply_placement_this_frame = false);

private:
    void RenderTopBar(AnsiCanvas* active_canvas, SessionState* session);
    void RenderGrid(AnsiCanvas* active_canvas, SessionState* session);
    void LoadFromSessionIfNeeded(SessionState* session);
    void SaveToSession(SessionState* session) const;

private:
    std::vector<Entry> entries_;
    int selected_ = -1;
    AnsiCanvas* last_active_canvas_ = nullptr;
    bool loaded_from_session_ = false;

    // UI state
    bool capture_composite_ = true;
    int thumb_px_ = 72;
    char new_name_buf_[128] = {};
};


