#pragma once

#include <cstdint>

namespace phos::colour
{
static constexpr std::uint16_t kUnsetIndex = 0xFFFFu;

struct ColourIndex
{
    std::uint16_t v = kUnsetIndex;
    bool IsUnset() const { return v == kUnsetIndex; }
};

// Plane-specific wrappers (same sentinel, different semantic meaning).
struct FgIndex { ColourIndex v; };
struct BgIndex { ColourIndex v; };

} // namespace phos::colour


