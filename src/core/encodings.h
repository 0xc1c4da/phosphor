#pragma once

#include <cstdint>

namespace phos::encodings
{
// OEM/DOS codepages used to interpret bitmap-font glyph indices as "bytes with meaning".
//
// Important distinction:
// - The canvas may store glyphs as BitmapIndex tokens (stable indices) with no encoding semantics.
// - This encoding model is primarily used at boundaries where we need Unicode <-> byte mapping:
//   - mapping a UnicodeScalar glyph into a bitmap font's index space
//   - export/import where text encoding is explicitly chosen
enum class EncodingId : std::uint8_t
{
    Cp437 = 0,
    Cp850,
    Cp852,
    Cp855,
    Cp857,
    Cp860,
    Cp861,
    Cp862,
    Cp863,
    Cp865,
    Cp866,
    Cp775,
    Cp737,
    Cp869,
    // AmigaOS baseline Latin-1 (ISO-8859-1) with a Topaz-style patch: 0x7F -> U+2302 (HOUSE).
    AmigaLatin1,
    // Amiga-flavored ISO-8859-* variants (useful for locale-specific text semantics).
    AmigaIso8859_15,
    AmigaIso8859_2,
    // Amiga-1251 (Cyrillic, Amiga) from references/mappings/Amiga-1251.txt.
    Amiga1251,
};

// Forward mapping: byte (0..255) -> Unicode representative codepoint.
// Always returns a value (undefined bytes may map to U+FFFD).
char32_t ByteToUnicode(EncodingId enc, std::uint8_t b);

// Reverse mapping: Unicode codepoint -> byte (0..255) if representable in the encoding.
bool UnicodeToByte(EncodingId enc, char32_t cp, std::uint8_t& out_b);
} // namespace phos::encodings


