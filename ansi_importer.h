#pragma once

#include "canvas.h"

#include <string>

namespace ansi_importer
{
struct Options
{
    // Logical column count. Most ANSI art targets 80.
    int columns = 80;

    // If true, SGR 5 (blink) is interpreted as "bright background" (ICE colors),
    // matching common ANSI art conventions.
    bool icecolors = true;

    // Default colors used when the file resets attributes (SGR 0 / 39 / 49).
    // These are stored as actual packed colors (not xterm indices).
    AnsiCanvas::Color32 default_fg = 0; // if 0, importer will use ANSI light gray
    AnsiCanvas::Color32 default_bg = 0; // if 0, importer will use ANSI black

    // Text decoding:
    // - If true (default), importer prefers CP437 but will auto-switch to UTF-8 when the
    //   byte stream strongly resembles valid UTF-8 and contains no ANSI escape sequences.
    // - If false, importer always decodes text as UTF-8.
    //
    // Rationale: classic .ANS files are typically CP437, but this editor also keeps UTF-8
    // demo art (e.g. `test.ans`) that should render as Unicode.
    bool cp437 = true;
};

// Import an ANSI (.ans) file into a new AnsiCanvas.
// Produces a single-layer canvas sized to `options.columns` x detected rows.
bool ImportAnsiFileToCanvas(const std::string& path,
                            AnsiCanvas& out_canvas,
                            std::string& err,
                            const Options& options = {});
} // namespace ansi_importer


