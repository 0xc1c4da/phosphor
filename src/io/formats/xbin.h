#pragma once

#include "core/canvas.h"
#include "io/formats/sauce.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// XBin (.XB) format module (import/export).
//
// XBin is an "extended BIN" container used in ANSI art tooling. It is a binary
// textmode format with explicit width/height, optional 16-color palette, optional
// 8xN bitmap font, and optional row-wise RLE compression.
//
// Spec references:
// - references/xbin-specification.html
// - references/xbin-tutorial.html
// - references/xbin-what-is.html
namespace formats
{
namespace xbin
{
// ---------------------------------------------------------------------------
// File extensions (single source of truth for UI/dispatch)
// ---------------------------------------------------------------------------
// Lowercase extensions (no leading dot).
const std::vector<std::string_view>& ImportExtensions();
const std::vector<std::string_view>& ExportExtensions();

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
struct ImportOptions
{
    // Decode the character byte as CP437 (recommended / classic XBin semantics).
    // If false, bytes >= 0x80 are mapped to U+FFFD.
    bool decode_cp437 = true;
};

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options = {});

bool ImportFileToCanvas(const std::string& path,
                        AnsiCanvas& out_canvas,
                        std::string& err,
                        const ImportOptions& options = {});

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------
struct ExportOptions
{
    enum class Source
    {
        // Export the composited "what you see" result (visible layers composited; spaces are transparent).
        Composite = 0,
        // Export only the active layer.
        ActiveLayer,
    };
    Source source = Source::Composite;

    // If true, write a 16-color palette chunk (recommended for deterministic colors).
    // Palette entries are stored as 6-bit (0..63) VGA-style components.
    bool include_palette = true;

    // If true, embed the canvas' current embedded bitmap font (if any) into the XBin.
    // Export will fail if the canvas does not have an embedded font.
    bool include_font = false;

    // If true, write compressed image data (XBin RLE). If false, write raw pairs.
    bool compress = true;

    // If true, set XBin "NonBlink" flag, meaning attribute bit7 is background intensity
    // (iCE / 16 background colors). Recommended, because it preserves 16 bg colors.
    bool nonblink = true;

    // XBin 512-character mode is not currently supported for export.
    bool mode_512 = false;

    // SAUCE append (optional; many XBin files omit SAUCE since geometry is in the header).
    bool write_sauce = false;
    sauce::WriteOptions sauce_write_options = {};
};

bool ExportCanvasToBytes(const AnsiCanvas& canvas,
                         std::vector<std::uint8_t>& out_bytes,
                         std::string& err,
                         const ExportOptions& options = {});

bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options = {});
} // namespace xbin
} // namespace formats


