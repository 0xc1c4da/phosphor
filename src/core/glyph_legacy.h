#pragma once

// Legacy glyph representations and migration helpers.
//
// Phosphor historically encoded embedded bitmap-font glyph indices using the Unicode Private Use Area:
//   U+E000 + glyph_index
//
// Internal storage is now `phos::GlyphId` tokens, but we still accept this legacy form on load /
// compatibility boundaries. Keep the "PUA decode" rule centralized here to avoid scattering
// raw range checks throughout the codebase.

#include <cstdint>
#include <limits>
#include <optional>

namespace phos::glyph
{
// Base codepoint for legacy embedded-glyph encoding (PUA).
static constexpr char32_t kLegacyEmbeddedPuaBase = (char32_t)0xE000;

// If `cp` is a legacy embedded PUA codepoint in the range [U+E000, U+E000 + glyph_count),
// return the decoded glyph index. Otherwise returns nullopt.
inline std::optional<std::uint16_t> TryDecodeLegacyEmbeddedPuaCodePoint(char32_t cp,
                                                                        std::uint32_t glyph_count)
{
    if (glyph_count == 0)
        return std::nullopt;
    if (cp < kLegacyEmbeddedPuaBase)
        return std::nullopt;
    const std::uint32_t delta = (std::uint32_t)(cp - kLegacyEmbeddedPuaBase);
    if (delta >= glyph_count)
        return std::nullopt;
    if (delta > (std::uint32_t)std::numeric_limits<std::uint16_t>::max())
        return std::nullopt;
    return (std::uint16_t)delta;
}
} // namespace phos::glyph


