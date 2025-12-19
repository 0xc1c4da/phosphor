#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas_rasterizer
{
struct Options
{
    // Integer scale factor applied to the base text-mode cell size (8x16 for Unscii at 16px).
    // Final output dimensions:
    //   out_w = columns * cell_w * scale
    //   out_h = rows    * cell_h * scale
    int scale = 2;

    // If true, treat "unset background" (bg==0) as transparent in the output image.
    // If false, use the editor theme paper color (white/black) as background.
    bool transparent_unset_bg = false;
};

// Rasterize the composited canvas (all visible layers) into an RGBA8 image.
// This uses Dear ImGui's font atlas for glyph bitmaps and assumes a monospaced Unscii-like font.
//
// NOTE: must be called after ImGui is initialized (needs font atlas texture data).
bool RasterizeCompositeToRgba32(const AnsiCanvas& canvas,
                               std::vector<std::uint8_t>& out_rgba,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const Options& opt = {});
} // namespace canvas_rasterizer


