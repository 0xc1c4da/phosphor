// Shared xterm-256 palette utilities (single source of truth).
// This is a built-in palette used as:
//   - a default palette identity for new canvases
//   - a common export target (ANSI/xterm modes, indexed image export)
//   - a fallback palette for legacy/boundary codepaths
//
// Alpha is intentionally not part of the model for the editor: colors are RGB only.
// The returned packed color is always opaque (A=255). A value of 0 is reserved by the
// canvas as "unset" (theme default / transparent bg), so callers should use 0 only
// for that semantic, not as a valid xterm color.
#pragma once

#include <cstdint>

namespace xterm256
{
struct Rgb
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Returns the palette RGB for idx (0..255). Out-of-range indices are clamped.
const Rgb& RgbForIndex(int idx);

// Returns a packed 32-bit color in Dear ImGui's IM_COL32 layout (ABGR, A=255).
// This is safe to store in AnsiCanvas::Color32 and to cast to ImU32.
std::uint32_t Color32ForIndex(int idx);

// Finds nearest xterm-256 index to the given RGB (0..255).
// This is designed to be much faster than scanning all 256 entries.
int NearestIndex(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Helpers
inline std::uint8_t ClampIndex(int idx)
{
    if (idx < 0) return 0;
    if (idx > 255) return 255;
    return (std::uint8_t)idx;
}
} // namespace xterm256


