#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

// Canonical ANSI format module (import/export).
//
// This file is the single authority for the ANSI "format backend":
// - how we decode ANSI-like byte streams into an AnsiCanvas
// - (future) how we encode an AnsiCanvas back into an ANSI-like byte stream
//
// Higher-level UI/IO code (IoManager, dialogs, etc.) should depend on this module,
// not on ad-hoc importer/exporter files.
namespace formats
{
namespace ansi
{
struct Options
{
    // Logical column count. Most ANSI art targets 80.
    // If <= 0, importer will default to 80 but may use SAUCE width when present.
    int columns = 80;

    // If true, SGR 5 (blink) is interpreted as "bright background" (ICE colors),
    // matching common ANSI art conventions.
    bool icecolors = true;

    // Default colors used when the file resets attributes (SGR 0 / 39 / 49).
    // These are stored as actual packed colors (not xterm indices).
    AnsiCanvas::Color32 default_fg = 0; // if 0, importer will use ANSI light gray
    AnsiCanvas::Color32 default_bg = 0; // if 0, importer will use ANSI black

    // If true, treat the "default background" as unset/transparent (Color32=0) instead
    // of forcing ANSI black. Useful for generated ANSI streams (e.g. Chafa) where
    // a default background should not paint over the editor UI.
    bool default_bg_unset = false;

    // Wrap behavior when the cursor reaches the last column.
    //
    // libansilove wraps before processing the next byte when in text state.
    // Some generated ANSI streams (e.g. Chafa) can include explicit newlines at
    // the row boundary, where eager wrapping can effectively double-advance.
    enum class WrapPolicy
    {
        // Match libansilove: when in text state and col==columns, advance to next row
        // before handling the next byte (including CSI sequences).
        LibAnsiLoveEager,
        // Only wrap when writing a printable glyph (i.e. via put()).
        PutOnly,
    };
    WrapPolicy wrap_policy = WrapPolicy::LibAnsiLoveEager;

    // Text decoding:
    // - If true (default), importer prefers CP437 but will auto-switch to UTF-8 when the
    //   byte stream strongly resembles valid UTF-8 and contains no ANSI escape sequences.
    // - If false, importer always decodes text as UTF-8.
    //
    // Rationale: classic .ANS files are typically CP437, but this editor also keeps UTF-8
    // demo art (e.g. `test.ans`) that should render as Unicode.
    bool cp437 = true;
};

// Import ANSI/UTF-8 byte stream into a new AnsiCanvas.
// This is the core importer used by ImportFileToCanvas(), exposed so callers can import ANSI
// generated in-memory (e.g. from Chafa) without writing temp files.
bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const Options& options = {});

// Import an ANSI (.ans) file into a new AnsiCanvas.
// Produces a single-layer canvas sized to `options.columns` x detected rows.
bool ImportFileToCanvas(const std::string& path,
                        AnsiCanvas& out_canvas,
                        std::string& err,
                        const Options& options = {});
} // namespace ansi
} // namespace formats


