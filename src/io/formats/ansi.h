#pragma once

#include "core/canvas.h"
#include "io/formats/sauce.h"

#include <cstdint>
#include <string>
#include <vector>

// Canonical ANSI format module (import/export).
//
// This file is the single authority for the ANSI "format backend":
// - how we decode ANSI-like byte streams into an AnsiCanvas
// - how we encode an AnsiCanvas back into an ANSI-like byte stream (with profiles/presets)
//
// Higher-level UI/IO code (IoManager, dialogs, etc.) should depend on this module,
// not on ad-hoc importer/exporter files.
namespace formats
{
namespace ansi
{
// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
struct ImportOptions
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
                         const ImportOptions& options = {});

// Import an ANSI (.ans) file into a new AnsiCanvas.
// Produces a single-layer canvas sized to `options.columns` x detected rows.
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

    // Text encoding of glyph bytes.
    enum class TextEncoding
    {
        Cp437 = 0,  // classic scene ANSI (8-bit bytes)
        Utf8,       // modern terminal output
        Utf8Bom,    // UTF-8 with BOM (Icy-style "unicode indicator")
    };
    TextEncoding text_encoding = TextEncoding::Utf8;

    // Newline policy (row separation).
    enum class Newline
    {
        CRLF = 0, // scene-friendly
        LF,       // terminal-friendly
    };
    Newline newline = Newline::LF;

    // Screen preparation emitted before content.
    enum class ScreenPrep
    {
        None = 0,
        ClearScreen, // ESC[2J
        Home,        // ESC[H
        ClearAndHome,
    };
    ScreenPrep screen_prep = ScreenPrep::None;

    // How colors are emitted.
    enum class ColorMode
    {
        // Classic 16-color SGR (30-37/40-47) with optional bold/ICE conventions.
        Ansi16 = 0,
        // Xterm indexed colors: 38;5;n / 48;5;n.
        Xterm256,
        // Truecolor SGR: 38;2;r;g;b / 48;2;r;g;b.
        TrueColorSgr,
        // PabloDraw/Icy/libansilove extension: ESC[1;R;G;Bt / ESC[0;R;G;Bt.
        // Typically used as an *overlay* on top of an ANSI16 baseline for compatibility.
        TrueColorPabloT,
    };
    ColorMode color_mode = ColorMode::Xterm256;

    // Only meaningful for ColorMode::TrueColorPabloT:
    // - If true, emit an ANSI16 baseline (classic SGR) and only emit `...t` when a cell's
    //   intended color differs from that ANSI16 approximation.
    // - If false, emit only `...t` sequences (plus optional 39/49 resets for unset colors).
    bool pablo_t_with_ansi16_fallback = true;

    // Only meaningful for ColorMode::Ansi16: how to represent "bright" colors.
    enum class Ansi16Bright
    {
        // "Scene classic": bright foreground via SGR 1 (bold). Bright background via SGR 5 when icecolors=true.
        BoldAndIceBlink = 0,
        // Emit bright codes 90-97/100-107 when needed (more terminal-y; less scene-compatible).
        Sgr90_100,
    };
    Ansi16Bright ansi16_bright = Ansi16Bright::BoldAndIceBlink;

    // If true, interpret background 8..15 as iCE (blink bit repurposed as bright bg) for Ansi16 export.
    bool icecolors = true;

    // Default colors used when exporting "unset" (Color32==0) cells.
    // - If default_* is 0, exporter uses ANSI default (fg=7, bg=0) for Ansi16
    //   or leaves as default (39/49) for modern modes depending on flags below.
    AnsiCanvas::Color32 default_fg = 0;
    AnsiCanvas::Color32 default_bg = 0;

    // Background/foreground reset policy for unset colors.
    // If true, "unset" background prefers SGR 49 (default bg) instead of painting black.
    bool use_default_bg_49 = true;
    // If true, "unset" foreground prefers SGR 39 (default fg) instead of forcing 37.
    bool use_default_fg_39 = true;

    // Xterm palette portability knob:
    // If true and color_mode==Xterm256, remap palette indices 0..15 to a nearest stable index in 16..255
    // to avoid terminal-configurable low-16 palette differences (Chafa guidance: prefer 240 colors).
    bool xterm_240_safe = false;

    // Geometry contract:
    // - If true, always write exactly canvas width columns per row (no trimming).
    // - If false, the exporter may trim trailing "safe blanks" to reduce size.
    bool preserve_line_length = true;

    // Compression options (applied only when semantically safe).
    bool compress = true;
    bool use_cursor_forward = true; // use CSI Ps C for runs of safe spaces (Pablo/Icy style)

    // Always end output with a reset.
    bool final_reset = true; // ESC[0m

    // SAUCE: if true, append SAUCE (+ optional COMNT + optional EOF 0x1A).
    bool write_sauce = false;
    sauce::WriteOptions sauce_write_options = {};
};

// Export the canvas to an ANSI-like byte stream according to `options`.
// Returns false on error and sets `err`.
bool ExportCanvasToBytes(const AnsiCanvas& canvas,
                         std::vector<std::uint8_t>& out_bytes,
                         std::string& err,
                         const ExportOptions& options = {});

// Convenience wrapper that exports and writes to disk.
bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options = {});

// ---------------------------------------------------------------------------
// Presets (profiles) for interoperability targets
// ---------------------------------------------------------------------------
enum class PresetId : int
{
    // Generic Phosphor presets
    SceneClassic = 0,      // CP437 + Ansi16 + CRLF + SAUCE (optional by default in UI)
    ModernUtf8_240Safe,    // UTF-8 + xterm256 (16..255) + LF
    ModernUtf8_256,        // UTF-8 + xterm256 (0..255) + LF
    TruecolorSgr_Utf8,     // UTF-8 + 38;2/48;2 + LF
    TruecolorPabloT_Cp437, // CP437 + 16-color fallback + ...t truecolor enhancements

    // Named ecosystem presets (initial set; can grow without changing core export logic)
    Durdraw_Utf8_256,
    Moebius_Classic,
    PabloDraw_Classic,
    IcyDraw_Modern,
};

struct Preset
{
    PresetId id = PresetId::SceneClassic;
    const char* name = nullptr;
    const char* description = nullptr;
    ImportOptions import = {};
    ExportOptions export_ = {};
};

// Returns a stable list of presets (static storage; do not free).
const std::vector<Preset>& Presets();
// Returns nullptr if not found.
const Preset* FindPreset(PresetId id);
} // namespace ansi
} // namespace formats


