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
    Font_PC_80x25, // classic DOS font; rendered as 9x16 cells (8-bit data + VGA 9th col rule)
    Font_PC_80x50, // 8x8 (rendered as 9x8 cells)

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

    // Cell metrics in "bitmap pixel units" at nominal 16px scale.
    // For ImGuiAtlas fonts, these are advisory and may be 0.
    int cell_w = 0; // 8 or 9 for classic textmode
    int cell_h = 0; // 8 or 16

    // Bitmap data (only for Bitmap1bpp fonts). Format:
    // - 256 glyphs
    // - glyph-major
    // - one byte per row
    // - MSB is leftmost pixel
    const std::uint8_t* bitmap = nullptr;

    // VGA 9th-column duplication (applies when cell_w==9).
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


