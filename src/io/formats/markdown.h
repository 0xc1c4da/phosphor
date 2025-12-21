#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Markdown format module (import into AnsiCanvas).
//
// This module is responsible for converting Markdown text into a styled AnsiCanvas using:
// - md4c for parsing correctness (callbacks)
// - Phosphor markdown theme JSON (assets/md-styles/*.json) for styling
//
// NOTE: This is an "importer" (Markdown -> canvas). Export is out of scope for now.
namespace formats
{
namespace markdown
{
// ---------------------------------------------------------------------------
// File extensions (single source of truth for UI/dispatch)
// ---------------------------------------------------------------------------
// Lowercase extensions (no leading dot).
const std::vector<std::string_view>& ImportExtensions();

// ---------------------------------------------------------------------------
// Themes (Phosphor Markdown Style JSON)
// ---------------------------------------------------------------------------
struct ThemeInfo
{
    std::string path;   // absolute theme file path in extracted assets dir
    std::string name;   // theme.name
    std::string author; // optional
};

// Scans the extracted assets directory for built-in themes:
//   assets/md-styles/*.json
bool ListBuiltinThemes(std::vector<ThemeInfo>& out, std::string& err);

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
struct ImportOptions
{
    // Canvas geometry.
    int columns = 80;       // clamp 20..400 in UI; backend clamps for safety too
    int max_rows = 10000;   // cap to bound memory use
    bool preserve_blank_lines = true;

    // Wrapping behavior.
    bool wrap_paragraphs = true;
    enum class SoftBreak
    {
        Space = 0,
        Newline,
    };
    SoftBreak soft_break = SoftBreak::Space;

    // Theme selection:
    // - if empty, the importer will attempt to load the default built-in theme
    //   (currently "dark.json"), and will fall back to a minimal theme on failure.
    std::string theme_path;

    // Links.
    enum class LinkMode
    {
        TextOnly = 0,  // render only link label
        InlineUrl,     // render "label (url)"
        // Footnotes is reserved for future (collect URLs and append section).
    };
    LinkMode link_mode = LinkMode::TextOnly;

    // Code blocks.
    bool show_code_language = true;

    // Horizontal rules.
    char32_t hr_glyph = U'─'; // fallback: '-' if font doesn't have it

    // Safety limits.
    std::size_t max_input_bytes = 2u * 1024u * 1024u; // default 2 MiB
};

bool ImportMarkdownToCanvas(std::string_view markdown_utf8,
                           AnsiCanvas& out_canvas,
                           std::string& err,
                           const ImportOptions& opt = {});
} // namespace markdown
} // namespace formats


