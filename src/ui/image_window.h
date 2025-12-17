#pragma once

#include <string>
#include <vector>

class ImageToChafaDialog;
struct SessionState;

// Simple representation of an imported image window (pixel buffer + metadata).
// The pixels are stored as RGBA8, row-major, width * height * 4 bytes.
struct ImageWindow
{
    bool        open   = true;
    int         id     = 0;
    std::string path;          // Original file path (for future ANSI conversion with chafa)

    // Raw pixel data owned by us: RGBA8, row-major, width * height * 4 bytes.
    int                       width  = 0;
    int                       height = 0;
    std::vector<unsigned char> pixels; // 4 bytes per pixel: R, G, B, A
};

// Render a single image window (metadata + scalable preview + context menu).
// Returns true if it was drawn (visible).
bool RenderImageWindow(const char* title, ImageWindow& image, ImageToChafaDialog& dialog,
                       SessionState* session = nullptr, bool apply_placement_this_frame = false);


