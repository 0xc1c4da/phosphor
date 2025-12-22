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

// Compute raster output dimensions for RasterizeCompositeToRgba32() without doing the rasterization.
// This is cheap and suitable for UI previews.
bool ComputeCompositeRasterSize(const AnsiCanvas& canvas,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const Options& opt = {});

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

// Rasterize a rectangular region (in *cell* coordinates) of the composited canvas into an RGBA8 image.
// The output pixels match RasterizeCompositeToRgba32() semantics exactly, but the origin is the
// top-left of `cell_rect`.
//
// Notes:
// - `cell_rect` is clamped to the canvas bounds.
// - Returns false if the resulting rect is empty.
bool ComputeCompositeRegionRasterSize(const AnsiCanvas& canvas,
                                     const AnsiCanvas::Rect& cell_rect,
                                     int& out_w,
                                     int& out_h,
                                     std::string& err,
                                     const Options& opt = {});

bool RasterizeCompositeRegionToRgba32(const AnsiCanvas& canvas,
                                     const AnsiCanvas::Rect& cell_rect,
                                     std::vector<std::uint8_t>& out_rgba,
                                     int& out_w,
                                     int& out_h,
                                     std::string& err,
                                     const Options& opt = {});

// Rasterize a rectangular region (in *cell* coordinates) of a single layer into an RGBA8 image.
// This uses the same rendering semantics as RasterizeCompositeToRgba32() but samples a specific layer
// instead of compositing all visible layers.
//
// - `layer_index` is the canvas layer index.
// - `cell_rect` is clamped to the canvas bounds.
// - Returns false if the resulting rect is empty or layer_index invalid.
bool RasterizeLayerRegionToRgba32(const AnsiCanvas& canvas,
                                 int layer_index,
                                 const AnsiCanvas::Rect& cell_rect,
                                 std::vector<std::uint8_t>& out_rgba,
                                 int& out_w,
                                 int& out_h,
                                 std::string& err,
                                 const Options& opt = {});
} // namespace canvas_rasterizer


