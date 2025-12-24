#pragma once

#include <cstdint>

#include "core/glyph_id.h"

// A "glyph selection" token used by UI components.
//
// We need to represent both:
// - Unicode codepoints (normal UTF-8 editing flow)
// - Bitmap font glyph indices (0..255), where the glyph is addressed by index.
// - Embedded-font glyph indices (XBIN), where the glyph is addressed by index.
struct GlyphToken
{
    enum class Kind : std::uint8_t
    {
        UnicodeCodePoint = 0,
        BitmapGlyphIndex,
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

    static GlyphToken BitmapIndex(uint32_t glyph_index)
    {
        GlyphToken t;
        t.kind = Kind::BitmapGlyphIndex;
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
    bool IsBitmapIndex() const { return kind == Kind::BitmapGlyphIndex; }
    bool IsEmbeddedIndex() const { return kind == Kind::EmbeddedGlyphIndex; }

    // Convert to the GlyphId token stored on the canvas (lossless).
    phos::GlyphId ToGlyphId() const
    {
        if (kind == Kind::EmbeddedGlyphIndex)
            return phos::glyph::MakeEmbeddedIndex((std::uint16_t)value);
        if (kind == Kind::BitmapGlyphIndex)
            return phos::glyph::MakeBitmapIndex((std::uint16_t)value);
        return phos::glyph::MakeUnicodeScalar((char32_t)value);
    }
};


