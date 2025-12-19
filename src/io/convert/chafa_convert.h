// Chafa-based RGBA -> ANSI (Utf-8 + escapes) conversion helpers.
#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chafa_convert
{
struct ImageRgba
{
    std::string               label; // path or friendly name
    int                       width = 0;
    int                       height = 0;
    int                       rowstride = 0; // bytes per row (>= width*4)
    std::vector<std::uint8_t> pixels; // RGBA8, unassociated alpha
};

struct Settings
{
    // --- Size & layout ---
    int  out_cols = 80;
    bool auto_rows = true;
    int  out_rows = 0; // used only when auto_rows=false

    // font_ratio = font_width / font_height (terminal cell aspect correction)
    // Typical terminals are taller than wide, so ~0.5 is a decent default.
    float font_ratio = 0.5f;
    bool  zoom = false;
    bool  stretch = false;

    // --- Color & processing ---
    // Canvas mode (UI enum index). This project targets xterm-indexed output (<=256 colors),
    // so we intentionally do not expose truecolor here.
    // 0=Indexed 256, 1=Indexed 240, 2=Indexed 16, 3=Indexed 16/8, 4=Indexed 8, 5=Default fg/bg + invert, 6=Default fg/bg (no codes)
    int canvas_mode = 0; // default: Indexed 256 (xterm)

    int color_extractor = 0; // 0=average, 1=median
    int color_space = 0;     // 0=rgb, 1=din99d

    // When enabled, set explicit display fg/bg colors (packed RGB, 0xRRGGBB).
    bool     use_custom_fg_bg = false;
    uint32_t fg_rgb = 0xFFFFFF;
    uint32_t bg_rgb = 0x000000;
    bool     invert_fg_bg = false;

    bool  preprocessing = true;
    float transparency_threshold = 0.0f; // UI semantics: 0=no extra transparency, 1=everything transparent

    // --- Symbols ---
    // Preset symbol tags (subset of Chafa symbol tags). Used when symbols_selectors is empty.
    int symbol_preset = 0; // 0=All, 1=Blocks, 2=ASCII, 3=Braille

    // Optional: selector syntax identical to chafa CLI (e.g. "block+border-diagonal").
    std::string symbols_selectors;
    std::string fill_selectors; // empty => defaults to symbols selection

    // --- Dithering ---
    int   dither_mode = 2; // 0=None,1=Ordered,2=Diffusion,3=Noise
    int   dither_grain = 4; // 1,2,4,8 (grain size in 1/8ths of a cell)
    float dither_intensity = 1.0f; // 0..inf (CLI allows >1)

    // --- Performance ---
    int   threads = -1;      // <0 = auto, 1.. = explicit
    int   work = 5;          // 1..9 (CLI-style). Mapped to libchafa's work_factor [0..1].

    // --- Output tweaks (symbols mode only) ---
    bool fg_only = false;

    // --- Debugging ---
    bool debug_stdout = false;        // print conversion diagnostics to stdout on regen
    bool debug_dump_raw_ansi = false; // WARNING: prints raw ANSI escapes to stdout (may garble terminal)
};

// Converts src RGBA to an AnsiCanvas using libchafa (symbols output) then imports the emitted
// UTF-8 + escape stream through the project's ANSI importer to ensure consistent behavior.
bool ConvertRgbaToAnsiCanvas(const ImageRgba& src, const Settings& s, AnsiCanvas& out, std::string& out_err);
} // namespace chafa_convert


