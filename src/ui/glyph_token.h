#pragma once

#include <cstdint>

#include "core/canvas.h"

// A "glyph selection" token used by UI components.
//
// We need to represent both:
// - Unicode codepoints (normal UTF-8 editing flow)
// - Embedded-font glyph indices (XBIN), where the glyph is addressed by index.
//
// Embedded glyph indices are represented on the canvas as Private Use Area codepoints:
//   U+E000 + glyph_index
struct GlyphToken
{
    enum class Kind : std::uint8_t
    {
        UnicodeCodePoint = 0,
        EmbeddedGlyphIndex,
    };

    Kind     kind = Kind::UnicodeCodePoint;
    uint32_t value = 0; // Unicode codepoint, or embedded glyph index

    static GlyphToken Unicode(uint32_t cp)
    {
        GlyphToken t;
        t.kind = Kind::UnicodeCodePoint;
        t.value = cp;
        return t;
    }

    static GlyphToken EmbeddedIndex(uint32_t glyph_index)
    {
        GlyphToken t;
        t.kind = Kind::EmbeddedGlyphIndex;
        t.value = glyph_index;
        return t;
    }

    bool IsValid() const
    {
        if (kind == Kind::UnicodeCodePoint)
            return value != 0;
        // glyph index 0 is valid (often blank), but we still treat 0 as valid here.
        return true;
    }

    bool IsUnicode() const { return kind == Kind::UnicodeCodePoint; }

    // Convert to the codepoint stored on the canvas.
    // - Unicode stays Unicode
    // - Embedded index becomes a PUA codepoint: U+E000 + index
    char32_t ToCanvasCodePoint() const
    {
        if (kind == Kind::EmbeddedGlyphIndex)
            return (char32_t)(AnsiCanvas::kEmbeddedGlyphBase + (char32_t)value);
        return (char32_t)value;
    }
};


