// Layer manager UI component for utf8-art-editor.
// Keeps layer GUI logic out of AnsiCanvas so the canvas stays focused on:
//  - data model (grid + layers)
//  - rendering the grid
//  - editing primitives
//
// The LayerManager window can target one of multiple canvases.

#pragma once

#include <vector>

class AnsiCanvas;

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
                const std::vector<LayerManagerCanvasRef>& canvases);

private:
    int selected_canvas_id_ = 0;
};


