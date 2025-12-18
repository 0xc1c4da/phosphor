// Minimap window for the current canvas.
//
// Shows a small overview of the whole canvas and a rectangle representing the
// current visible viewport. The rectangle can be dragged to pan the canvas.
// Mouse wheel zooms the canvas in/out.
// NOTE:
// The window can render either:
//  - a Vulkan-backed minimap texture (preferred, higher quality), or
//  - a fallback immediate-mode sampled grid (no texture plumbing required).

#pragma once

class AnsiCanvas;
struct SessionState;
struct CanvasPreviewTextureView;

class MinimapWindow
{
public:
    MinimapWindow() = default;

    // Render the minimap window. Returns true if it was drawn (visible).
    bool Render(const char* title, bool* p_open, AnsiCanvas* canvas,
                const CanvasPreviewTextureView* minimap_texture,
                SessionState* session = nullptr, bool apply_placement_this_frame = false);

private:
    // Drag interaction state.
    bool  m_dragging = false;
    float m_drag_off_x = 0.0f; // mouse - rect_min in minimap space
    float m_drag_off_y = 0.0f;
};


