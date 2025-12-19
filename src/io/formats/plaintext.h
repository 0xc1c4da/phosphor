#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Plaintext format module (import/export).
//
// "Plaintext" here means: a byte stream that contains only text and newlines.
// - Import ignores ANSI escape sequences (ESC is treated as a control char and skipped).
// - Export emits only glyph bytes + newlines (no SGR, no cursor movement, no SAUCE).
//
// This is intentionally separate from formats::ansi:
// - formats::ansi may *import* plain text as a subset, but ANSI export is not plaintext.
// - Keeping plaintext as its own format makes dispatch and UI wording unambiguous.
namespace formats
{
namespace plaintext
{
// ---------------------------------------------------------------------------
// File extensions (single source of truth for UI/dispatch)
// ---------------------------------------------------------------------------
// Lowercase extensions (no leading dot).
// Note: extensions are hints; content may still include ANSI escapes, but plaintext
// exporters never emit color/control sequences.
const std::vector<std::string_view>& ImportExtensions();
const std::vector<std::string_view>& ExportExtensions();

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
struct ImportOptions
{
    // Logical column count. Long lines hard-wrap at this width.
    int columns = 80;

    enum class TextEncoding
    {
        // Treat bytes as UTF-8 (optionally with BOM). Malformed sequences are skipped.
        Utf8 = 0,
        // Treat bytes as 7-bit ASCII. Bytes >= 0x80 become '?'.
        Ascii,
    };
    TextEncoding text_encoding = TextEncoding::Utf8;

    // Newline handling:
    // - CR and LF both act as newlines.
    // - CRLF is normalized (LF after CR is ignored).
    bool normalize_crlf = true;

    // Replace tab with a single space (matches AnsiCanvas::LoadFromFile()).
    bool tab_to_space = true;

    // If true, ASCII control chars (< 0x20) other than tab/newline are ignored.
    bool filter_control_chars = true;
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
        Composite = 0,
        ActiveLayer,
    };
    Source source = Source::Composite;

    enum class TextEncoding
    {
        Ascii = 0,
        Utf8,
        Utf8Bom,
    };
    TextEncoding text_encoding = TextEncoding::Utf8;

    enum class Newline
    {
        CRLF = 0,
        LF,
    };
    Newline newline = Newline::LF;

    // If true, always write exactly canvas width columns per row (no trimming).
    // If false, trailing blank-ish cells are trimmed (space / NUL / 0xFF).
    bool preserve_line_length = true;

    // If true, always end output with a newline for the last exported row.
    // (When preserve_line_length=false and the last row trims to empty, this still emits a newline.)
    bool final_newline = true;
};

bool ExportCanvasToBytes(const AnsiCanvas& canvas,
                         std::vector<std::uint8_t>& out_bytes,
                         std::string& err,
                         const ExportOptions& options = {});

bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options = {});

// ---------------------------------------------------------------------------
// Presets (profiles)
// ---------------------------------------------------------------------------
enum class PresetId : int
{
    PlainUtf8 = 0,
    PlainUtf8Bom,
    PlainAscii,
};

struct Preset
{
    PresetId id = PresetId::PlainUtf8;
    const char* name = nullptr;
    const char* description = nullptr;
    ImportOptions import = {};
    ExportOptions export_ = {};
};

const std::vector<Preset>& Presets();
const Preset* FindPreset(PresetId id);
} // namespace plaintext
} // namespace formats


