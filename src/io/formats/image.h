#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace formats
{
namespace image
{
// ---------------------------------------------------------------------------
// File extensions (single source of truth for UI/dispatch)
// ---------------------------------------------------------------------------
// Lowercase extensions (no leading dot).
const std::vector<std::string_view>& ImportExtensions();
const std::vector<std::string_view>& ExportExtensions();

// ---------------------------------------------------------------------------
// Import (images -> RGBA)
// ---------------------------------------------------------------------------
struct RgbaImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8
};

// NOTE: This returns image pixels (not an AnsiCanvas). Converting image->ANSI is handled
// by Chafa + formats::ansi in the UI (see ImageToChafaDialog).
bool ImportFileToRgba(const std::string& path, RgbaImage& out, std::string& err);

// ---------------------------------------------------------------------------
// Export (canvas -> image file)
// ---------------------------------------------------------------------------
struct ExportOptions
{
    // Integer scale applied to the base 8x16 cell size (derived from ImGui font).
    // User does not select explicit output dimensions; they select the scale.
    int scale = 2;

    // Background policy: if true, bg==0 becomes transparent.
    bool transparent_unset_bg = false;

    // PNG bit depth mode:
    // - 24: RGB truecolor (opaque)
    // - 32: RGBA truecolor
    // - 8:  indexed palette (xterm256 quantized)
    // - 4:  indexed palette (xterm16 quantized)
    int png_bit_depth = 32;

    // PNG compression level (lodepng zlib setting; 0..9).
    int png_compression = 6;

    // JPEG quality 1..100.
    int jpg_quality = 95;
};

bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options = {});
} // namespace image
} // namespace formats


