#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace textmode_font
{
// ---------------------------------------------------------------------------
// Public surface (text-mode art font backend):
// - Detect + parse FIGlet (.flf) and TheDraw (.tdf) fonts from bytes
// - Render text into a cell grid (Unicode codepoints + optional per-cell colors)
// ---------------------------------------------------------------------------

enum class Kind : std::uint8_t
{
    Figlet = 0,
    Tdf,
};

enum class TdfFontType : std::uint8_t
{
    Outline = 0,
    Block   = 1,
    Color   = 2,
};

enum class RenderMode : std::uint8_t
{
    Display = 0,
    Edit,
};

struct RenderOptions
{
    RenderMode mode = RenderMode::Display;
    // 0..18 inclusive; out-of-range falls back to CP437 placeholder bytes (Rust behavior).
    int outline_style = 0;

    // If true, and the glyph stream contains per-cell colors (TDF Color fonts),
    // the renderer emits those colors. If false, colors are left unset (0) and
    // callers can stamp their own fg/bg.
    bool use_font_colors = true;

    // If true, TDF blink bit is treated as "bright background" (ICE colors),
    // matching the rest of the editor's ANSI conventions.
    bool icecolors = true;
};

// A rendered cell grid.
// - `fg`/`bg` are packed 32-bit colors in Dear ImGui's IM_COL32 ABGR layout.
// - 0 means "unset" (use theme default / transparent bg), matching AnsiCanvas semantics.
struct Bitmap
{
    int w = 0;
    int h = 0;
    std::vector<char32_t>  cp; // row-major, size w*h
    std::vector<std::uint32_t> fg; // row-major, size w*h
    std::vector<std::uint32_t> bg; // row-major, size w*h
};

struct FontMeta
{
    Kind kind = Kind::Figlet;
    std::string name;
    TdfFontType tdf_type = TdfFontType::Block; // only meaningful when kind==Tdf
    int spacing = 1; // only meaningful for TDF (and as a hint for FIGlet space fallback)
};

// Opaque font handle (PIMPL) so the parser/renderer can stay self-contained without
// leaking all internal tables into headers.
struct Font
{
    Kind kind = Kind::Figlet;
    struct Impl;
    std::shared_ptr<Impl> impl;
};

// Load a FIGlet font (returns exactly 1 font) or a TDF bundle (returns 1+ fonts).
// Accepts extensionless inputs (detects by magic bytes).
bool LoadFontsFromBytes(const std::vector<std::uint8_t>& bytes,
                        std::vector<Font>& out_fonts,
                        std::string& err);

// Render UTF-8 text using a previously loaded font.
// The output bitmap is tightly sized to the rendered content.
bool RenderText(const struct Font& font,
                std::string_view utf8_text,
                const RenderOptions& options,
                Bitmap& out,
                std::string& err);

// Query metadata without re-parsing.
FontMeta GetMeta(const struct Font& font);
} // namespace textmode_font


