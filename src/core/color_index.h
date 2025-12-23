#pragma once

#include <cstdint>

namespace phos::color
{
static constexpr std::uint16_t kUnsetIndex = 0xFFFFu;

struct ColorIndex
{
    std::uint16_t v = kUnsetIndex;
    bool IsUnset() const { return v == kUnsetIndex; }
};

// Plane-specific wrappers (same sentinel, different semantic meaning).
struct FgIndex { ColorIndex v; };
struct BgIndex { ColorIndex v; };

} // namespace phos::color


