#pragma once

// Shared helpers to resolve GlyphId tokens into either:
// - a Unicode scalar representative (for ImGuiAtlas / UTF-8 display), or
// - a bitmap glyph index (for Bitmap1bpp / embedded font rendering).
//
// This centralizes the last remaining "legacy embedded PUA" compatibility behavior:
// Unicode scalars in the range [U+E000, U+E000+glyph_count) are treated as embedded glyph indices
// when an embedded font is present.

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/glyph_id.h"
#include "core/glyph_legacy.h"

#include <cstdint>
#include <optional>

namespace phos::glyph
{
// Deterministic Unicode representative for UI/text when the stored glyph is not Unicode.
// v1 policy:
// - BitmapIndex -> CP437 representative
// - EmbeddedIndex -> CP437 representative of low 8 bits (best-effort)
inline char32_t ToUnicodeRepresentative(phos::GlyphId g)
{
    if (IsUnicodeScalar(g))
        return ToUnicodeScalar(g);
    const Kind k = GetKind(g);
    if (k == Kind::BitmapIndex)
        return fonts::Cp437ByteToUnicode((std::uint8_t)BitmapIndexValue(g));
    if (k == Kind::EmbeddedIndex)
        return fonts::Cp437ByteToUnicode((std::uint8_t)(EmbeddedIndexValue(g) & 0xFFu));
    return U'?';
}

inline bool EmbeddedFontUsable(const AnsiCanvas::EmbeddedBitmapFont* ef)
{
    return ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
           ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h;
}

// Returns an embedded glyph index if `g` is:
// - an EmbeddedIndex token, or
// - a legacy embedded PUA Unicode scalar (U+E000 + index) and an embedded font exists.
inline std::optional<std::uint16_t> TryGetEmbeddedIndex(phos::GlyphId g,
                                                        const AnsiCanvas::EmbeddedBitmapFont* ef)
{
    if (!EmbeddedFontUsable(ef))
        return std::nullopt;

    const Kind k = GetKind(g);
    if (k == Kind::EmbeddedIndex)
        return (std::uint16_t)EmbeddedIndexValue(g);

    if (IsUnicodeScalar(g))
    {
        const char32_t cp = ToUnicodeScalar(g);
        return TryDecodeLegacyEmbeddedPuaCodePoint(cp, (std::uint32_t)ef->glyph_count);
    }

    return std::nullopt;
}

struct BitmapGlyphRef
{
    std::uint16_t glyph_index = 0;
    bool use_embedded = false; // if true, sample from embedded font bitmap; else sample from fonts::BitmapGlyphRowBits
};

// Resolve a GlyphId into a bitmap glyph index using the current canvas font context.
//
// - If an embedded font is usable, we always render using the embedded font bitmap table
//   (matching existing behavior), but glyph IDs may still be Unicode/BitmapIndex/EmbeddedIndex.
// - Without an embedded font, we render using the selected bitmap font (CP437 mapping today).
inline BitmapGlyphRef ResolveBitmapGlyph(const fonts::FontInfo& finfo,
                                        const AnsiCanvas::EmbeddedBitmapFont* ef,
                                        phos::GlyphId g)
{
    BitmapGlyphRef r{};
    r.use_embedded = EmbeddedFontUsable(ef);

    if (r.use_embedded)
    {
        if (auto idx = TryGetEmbeddedIndex(g, ef))
        {
            r.glyph_index = *idx;
            return r;
        }

        // In embedded-font mode, treat BitmapIndex as a direct table index as well.
        if (GetKind(g) == Kind::BitmapIndex)
        {
            r.glyph_index = (std::uint16_t)BitmapIndexValue(g);
            return r;
        }

        // UnicodeScalar (or other token kinds): best-effort map Unicode -> glyph index.
        const char32_t cp = ToUnicodeRepresentative(g);
        std::uint16_t gi = 0;
        if (!fonts::UnicodeToGlyphIndex(finfo.id, cp, gi))
        {
            if (!fonts::UnicodeToGlyphIndex(finfo.id, U'?', gi))
                gi = (std::uint16_t)' ';
        }
        r.glyph_index = gi;
        return r;
    }

    // Non-embedded bitmap font path:
    const Kind k = GetKind(g);
    if (k == Kind::BitmapIndex)
    {
        r.glyph_index = (std::uint16_t)BitmapIndexValue(g);
        return r;
    }
    if (k == Kind::EmbeddedIndex)
    {
        // Best-effort: treat embedded indices as plain bitmap indices when the table is missing.
        r.glyph_index = (std::uint16_t)EmbeddedIndexValue(g);
        return r;
    }

    const char32_t cp = ToUnicodeRepresentative(g);
    std::uint16_t gi = 0;
    if (!fonts::UnicodeToGlyphIndex(finfo.id, cp, gi))
    {
        if (!fonts::UnicodeToGlyphIndex(finfo.id, U'?', gi))
            gi = (std::uint16_t)' ';
    }
    r.glyph_index = gi;
    return r;
}
} // namespace phos::glyph


