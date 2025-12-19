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
    enum class PngFormat : int
    {
        Rgb24 = 24,   // RGB truecolor (opaque)
        Rgba32 = 32,  // RGBA truecolor
        // Indexed palette (default: xterm-256 quantized).
        //
        // Terminology:
        // - "xterm256" means the standard 256-color xterm palette indices 0..255.
        // - "xterm240" commonly refers to the "safe" subset indices 16..255 (i.e. avoiding 0..15),
        //   because terminals may remap the low 16 colors via user theme config.
        //
        // NOTE: Whether we quantize to full xterm256 vs the 240-safe subset is controlled by
        // `xterm_240_safe` below.
        Indexed8 = 8,

        // Indexed palette (16 colors).
        // This uses ANSI16 / iCE.
        Indexed4 = 4,
    };

    // Integer scale applied to the base 8x16 cell size (derived from ImGui font).
    // User does not select explicit output dimensions; they select the scale.
    int scale = 2;

    // Background policy: if true, bg==0 becomes transparent.
    bool transparent_unset_bg = false;

    // PNG format mode.
    //
    // Default is Indexed8.
    PngFormat png_format = PngFormat::Indexed8;

    // Only meaningful when png_format == Indexed8:
    // If true, quantize into the 240-color subset (xterm indices 16..255), avoiding 0..15.
    // This mirrors the "xterm_240_safe" idea used in ANSI export profiles.
    bool xterm_240_safe = false;

    // PNG compression level (lodepng zlib settings; 0..9).
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


