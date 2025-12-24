#pragma once

#include <cstdint>

namespace phos
{
// Opaque glyph token stored in canvas cells (Option B groundwork).
//
// Encoding v1:
// - 0x00000000..0x0010FFFF : Unicode scalar (stored directly)
// - 0x80000000..0xFFFFFFFF : Token space
//
// Token space layout:
// - bit 31: 1 (token marker)
// - bits 30..28: kind (3 bits)
// - bits 27..0: payload
using GlyphId = std::uint32_t;

namespace glyph
{
static constexpr GlyphId kTokenBit = 0x80000000u;
static constexpr GlyphId kKindMask = 0x70000000u; // bits 30..28
static constexpr int     kKindShift = 28;
static constexpr GlyphId kPayloadMask = 0x0FFFFFFFu;

enum class Kind : std::uint8_t
{
    UnicodeScalar = 0,
    BitmapIndex   = 1,
    EmbeddedIndex = 2,
};

constexpr bool IsToken(GlyphId g) { return (g & kTokenBit) != 0; }

constexpr Kind GetKind(GlyphId g)
{
    if (!IsToken(g))
        return Kind::UnicodeScalar;
    const std::uint32_t k = (g & kKindMask) >> kKindShift;
    switch (k)
    {
        case 1: return Kind::BitmapIndex;
        case 2: return Kind::EmbeddedIndex;
        default: return Kind::UnicodeScalar; // defensive
    }
}

constexpr bool IsUnicodeScalar(GlyphId g) { return !IsToken(g); }

constexpr char32_t ToUnicodeScalar(GlyphId g)
{
    return (char32_t)g;
}

// NOTE: v1 does not validate scalar range here; callers should sanitize at boundaries if needed.
constexpr GlyphId MakeUnicodeScalar(char32_t cp)
{
    return (GlyphId)cp;
}

// Payload helpers (v1: store index in low bits; future can expand).
static constexpr std::uint16_t kIndexMask = 0x0FFFu; // 12 bits (0..4095)

constexpr GlyphId MakeBitmapIndex(std::uint16_t idx)
{
    return kTokenBit | ((GlyphId)Kind::BitmapIndex << kKindShift) | (GlyphId)(idx & kIndexMask);
}

constexpr GlyphId MakeEmbeddedIndex(std::uint16_t idx)
{
    return kTokenBit | ((GlyphId)Kind::EmbeddedIndex << kKindShift) | (GlyphId)(idx & kIndexMask);
}

constexpr std::uint16_t BitmapIndexValue(GlyphId g)
{
    return (std::uint16_t)(g & kIndexMask);
}

constexpr std::uint16_t EmbeddedIndexValue(GlyphId g)
{
    return (std::uint16_t)(g & kIndexMask);
}

// Central "blank glyph" predicate (replaces cp==U' ' in compositing/paste/transparency).
// v1 policy:
// - UnicodeScalar: blank iff U' '
// - BitmapIndex: blank iff index == 32 (space)
// - EmbeddedIndex: blank iff index == 32 (space)
constexpr bool IsBlank(GlyphId g)
{
    if (IsUnicodeScalar(g))
        return ToUnicodeScalar(g) == U' ';
    const Kind k = GetKind(g);
    if (k == Kind::BitmapIndex)
        return BitmapIndexValue(g) == 32u;
    if (k == Kind::EmbeddedIndex)
        return EmbeddedIndexValue(g) == 32u;
    return false;
}
} // namespace glyph
} // namespace phos


