#pragma once

#include "core/canvas.h"
#include "core/encodings.h"
#include "io/formats/sauce.h"

#include <cstdint>
#include <string>
#include <string_view>
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
// File extensions (single source of truth for UI/dispatch)
// ---------------------------------------------------------------------------
// Lowercase extensions (no leading dot).
// Note: ANSI format can also *import* plain text files, but these lists describe
// the extensions we associate with this format in Phosphor's UI/dispatch layer.
const std::vector<std::string_view>& ImportExtensions();
const std::vector<std::string_view>& ExportExtensions();

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
struct ImportOptions
{
    // Logical column count (canvas width).
    //
    // Semantics:
    // - If > 0: treat as an explicit override (force this width).
    // - If <= 0: auto-width mode (prefer SAUCE width when present/valid; otherwise infer;
    //           fall back to 80 only if inference fails).
    //
    // Rationale: many ANSI art files rely on terminal wrapping and/or cursor positioning,
    // so forcing 80 by default can be wrong for SAUCE'd works (e.g. 100/132 cols).
    int columns = 0;

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
    // - If true (default), importer prefers 8-bit byte mode (classic scene ANSI) but may
    //   auto-switch to UTF-8 when the *text payload bytes* (ANSI sequences stripped) strongly
    //   resemble valid UTF-8, or when an explicit UTF-8 BOM is present.
    // - If false, importer always decodes text as UTF-8.
    //
    // SAUCE-first policy:
    // - If SAUCE declares a known font, we respect it when deciding UTF-8 vs 8-bit:
    //   - ImGuiAtlas fonts (e.g. Unscii) imply UTF-8.
    //   - Bitmap fonts imply 8-bit byte semantics (UTF-8 BOM still overrides).
    //
    // Rationale: classic .ANS files are typically byte-indexed; modern terminal ANSI may be UTF-8.
    bool cp437 = true;

    // When decoding ANSI "text bytes" as an 8-bit encoding (i.e. cp437==true and UTF-8 auto-detect
    // does not trigger), interpret bytes using this encoding table.
    //
    // Note: this affects *byte<->Unicode* mapping when we choose to decode bytes to Unicode.
    // If glyph_bytes_policy==StoreAsBitmapIndex, we preserve the original byte identity in
    // canvas cells as BitmapIndex tokens (lossless) and this encoding only affects best-effort
    // Unicode representatives at UI/text boundaries.
    phos::encodings::EncodingId byte_encoding = phos::encodings::EncodingId::Cp437;

    // Policy for how "text payload bytes" are stored in the canvas when importing an ANSI stream
    // in 8-bit byte mode.
    enum class GlyphBytesPolicy
    {
        // Preserve current behavior: decode bytes -> UnicodeScalar glyphs (potentially lossy).
        DecodeToUnicode = 0,
        // ANSI-art-friendly: store bytes as BitmapIndex glyph tokens (lossless index identity).
        StoreAsBitmapIndex,
    };
    // Default: lossless. Classic ANSI art is fundamentally byte-indexed; storing BitmapIndex preserves identity
    // and still renders correctly (representative Unicode is derived where needed).
    GlyphBytesPolicy glyph_bytes_policy = GlyphBytesPolicy::StoreAsBitmapIndex;
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
    // Which SGR attribute codes we will emit (independent of color mode).
    //
    // Rationale:
    // - "Classic" DOS-era drivers (ANSI.SYS / BBSes) widely supported only a small subset:
    //     0 reset, 1 bold/bright (often mapped to high-intensity colors), 5 blink (often used for iCE),
    //     7 reverse video.
    // - Modern terminals support the larger set (dim/italic/underline/strikethrough and per-attr resets).
    enum class AttributeMode
    {
        ClassicDos = 0,
        Modern,
    };
    AttributeMode attribute_mode = AttributeMode::Modern;

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
        Cp437 = 0,  // 8-bit byte stream (classic scene ANSI); see `byte_encoding`
        Utf8,       // modern terminal output
        Utf8Bom,    // UTF-8 with BOM (Icy-style "unicode indicator")
    };
    TextEncoding text_encoding = TextEncoding::Utf8;

    // When text_encoding==Cp437 (8-bit byte stream), choose which encoding table is used for
    // UnicodeScalar -> byte mapping (and for best-effort fallbacks).
    phos::encodings::EncodingId byte_encoding = phos::encodings::EncodingId::Cp437;

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


