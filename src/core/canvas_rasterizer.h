#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace canvas_rasterizer
{
struct Options
{
    // Integer scale factor applied to the base text-mode cell size.
    // Final output dimensions:
    //   out_w = columns * cell_w * scale
    //   out_h = rows    * cell_h * scale
    int scale = 2;

    // If true, treat "unset background" (bg==0) as transparent in the output image.
    // If false, use the canvas paper color (white/black).
    bool transparent_unset_bg = false;
};

// Rasterize the composited canvas (all visible layers) into an RGBA8 image.
//
// Font source:
// - For bitmap fonts (`fonts::Kind::Bitmap1bpp`), glyphs are taken from `fonts` and this function
//   does not require Dear ImGui to be initialized.
// - For atlas fonts (`fonts::Kind::ImGuiAtlas`), glyphs are sampled from the active ImGui font atlas
//   and therefore require ImGui to be initialized (font atlas uploaded/baked).
//
bool RasterizeCompositeToRgba32(const AnsiCanvas& canvas,
                               std::vector<std::uint8_t>& out_rgba,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const Options& opt = {});
} // namespace canvas_rasterizer


