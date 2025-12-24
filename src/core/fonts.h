#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// Core font registry + glyph lookup.
//
// This is the single source of truth for fonts supported by Phosphor's canvas rendering.
// UI may still use a separate ImGui TTF font (currently Unscii), but the canvas can render
// using either:
// - the active ImGui font (Unicode-capable, e.g. Unscii), or
// - embedded 1bpp bitmap fonts (libansilove-derived, CP437-ordered glyph tables).
namespace fonts
{
enum class Kind : std::uint8_t
{
    ImGuiAtlas = 0, // draw via ImGui AddText / atlas sampling (Unicode codepoints)
    Bitmap1bpp,     // draw via 1bpp glyph bitmaps (usually CP437-ordered, 256 glyphs)
};

enum class FontId : std::uint8_t
{
    Unscii = 0,

    // libansilove-derived bitmap fonts (CP437 glyph order)
    //
    // IMPORTANT:
    // The underlying bitmap tables are 8 pixels wide (1 byte per row).
    // Some DOS/VGA renderers treat the cell as 9 pixels wide and apply a special
    // 9th-column duplication rule for box/line glyphs (CP437 192..223).
    //
    // In Phosphor we treat the bitmap fonts by their actual bitmap width (8px)
    // to avoid introducing an always-empty spacer column for most glyphs (notably
    // the CP437 shading characters U+2591..U+2593).
    Font_PC_80x25, // classic DOS font; 8x16 bitmap
    Font_PC_80x50, // 8x8 bitmap

    // IBM PC OEM codepage fonts (libansilove-derived bitmap fonts).
    Font_PC_Latin1,
    Font_PC_Latin2,
    Font_PC_Cyrillic,
    Font_PC_Russian,
    Font_PC_Greek,
    Font_PC_Greek869,
    Font_PC_Turkish,
    Font_PC_Hebrew,
    Font_PC_Icelandic,
    Font_PC_Nordic,
    Font_PC_Portuguese,
    Font_PC_FrenchCanadian,
    Font_PC_Baltic,

    // Extra bitmap fonts (still treated as 256-glyph tables)
    Font_Terminus,
    Font_Spleen,

    // Amiga fonts (libansilove-derived)
    Font_Amiga_Topaz500,
    Font_Amiga_Topaz500Plus,
    Font_Amiga_Topaz1200,
    Font_Amiga_Topaz1200Plus,
    Font_Amiga_PotNoodle,
    Font_Amiga_Microknight,
    Font_Amiga_MicroknightPlus,
    Font_Amiga_Mosoul,
};

struct FontInfo
{
    FontId id = FontId::Unscii;
    Kind   kind = Kind::ImGuiAtlas;

    // UI label.
    const char* label = "";

    // SAUCE TInfoS / "FontName" (<= 22 bytes, ZString in the SAUCE record).
    //
    // SAUCE allows arbitrary FontName strings, but the spec documents a set of well-known
    // canonical names (e.g. "IBM VGA 437", "Amiga Topaz 2+"). We use these when possible.
    //
    // This value is also used as the on-disk identifier for the canvas font selection
    // (persisted via ProjectState::SauceMeta::tinfos, and written on .ans export).
    // Therefore: keep it stable across releases.
    const char* sauce_name = "";

    // Cell metrics in "bitmap pixel units".
    // For ImGuiAtlas fonts, these are advisory and may be 0.
    //
    // IMPORTANT (Bitmap1bpp fonts):
    // Our built-in bitmap font tables are stored as 1 byte per glyph row (8 pixels wide).
    // Therefore built-in Bitmap1bpp fonts must use cell_w == 8.
    //
    // libansilove (the upstream source of many of these tables) reports PC fonts as width=9, but
    // that 9th pixel column is *not* stored in the bitmap. It's a render-time rule:
    // duplicate the 8th column for CP437 192..223 when rendering with "bits==9".
    //
    // A legacy DOS/VGA "9th-column duplication" mode exists (for CP437 192..223 when cell_w==9),
    // but Phosphor intentionally does not use it for built-in fonts (to avoid a permanent spacer
    // column that is especially visible on shading glyphs U+2591..U+2593).
    int cell_w = 0; // built-in Bitmap1bpp fonts: 8
    int cell_h = 0; // 8 or 16

    // Bitmap data (only for Bitmap1bpp fonts). Format:
    // - 256 glyphs
    // - glyph-major
    // - one byte per row
    // - MSB is leftmost pixel
    const std::uint8_t* bitmap = nullptr;

    // VGA 9th-column duplication (legacy DOS behavior; only meaningful if cell_w==9).
    // NOTE: built-in Bitmap1bpp fonts ship with vga_9col_dup == false.
    bool vga_9col_dup = false;
};

// Default canvas font for new canvases (UI font remains Unscii regardless).
FontId DefaultCanvasFont();

// Registry
const std::vector<FontInfo>& AllFonts();
const FontInfo& Get(FontId id);

// SAUCE helpers (tinfos <-> FontId).
// Try to map a SAUCE TInfoS / FontName string to a known FontId.
// Returns false if the value is empty or unrecognized (no defaulting).
bool TryFromSauceName(std::string_view tinfos, FontId& out_id);
FontId FromSauceName(std::string_view tinfos);
std::string_view ToSauceName(FontId id);

// CP437 helpers (used by bitmap fonts).
char32_t Cp437ByteToUnicode(std::uint8_t b);
bool     UnicodeToCp437Byte(char32_t cp, std::uint8_t& out_b);

// Map a Unicode codepoint to a glyph index in a specific font.
// - For ImGuiAtlas fonts: returns true with out_glyph == cp if cp fits in 32-bit.
// - For CP437 bitmap fonts: maps Unicode -> CP437 byte -> glyph index (0..255).
// If unmappable, returns false (caller decides fallback).
bool UnicodeToGlyphIndex(FontId font, char32_t cp, std::uint16_t& out_glyph);

// Read the packed 8-bit row bits for a bitmap glyph.
// Returns 0 if not a bitmap font or out of range.
std::uint8_t BitmapGlyphRowBits(FontId font, std::uint16_t glyph_index, int row_y);
} // namespace fonts


