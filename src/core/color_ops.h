#pragma once

#include "core/color_index.h"
#include "core/palette/palette.h"

#include <cstdint>

namespace phos::color
{
// Low-level color operations that will become the backbone of the indexed-canvas refactor.
// For now, this is used at a few callsites as a bridge from packed ImGui-style ABGR to indices.
class ColorOps
{
public:
    // Packed color uses ImGui ABGR (A high byte, R low byte). A value of 0 is treated as "unset".
    static inline bool UnpackImGuiAbgr(std::uint32_t c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
    {
        if (c == 0)
            return false;
        r = (std::uint8_t)((c >> 0) & 0xFF);
        g = (std::uint8_t)((c >> 8) & 0xFF);
        b = (std::uint8_t)((c >> 16) & 0xFF);
        return true;
    }

    static inline std::uint32_t PackImGuiAbgrOpaque(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        return 0xFF000000u | ((std::uint32_t)b << 16) | ((std::uint32_t)g << 8) | (std::uint32_t)r;
    }

    // RGB -> nearest palette index (deterministic; ties -> lowest index).
    static std::uint8_t NearestIndexRgb(const PaletteRegistry& reg,
                                        PaletteInstanceId pal,
                                        std::uint8_t r,
                                        std::uint8_t g,
                                        std::uint8_t b,
                                        const QuantizePolicy& policy);

    // Packed ImGui ABGR (0==unset) -> ColorIndex (unset remains unset).
    static ColorIndex Color32ToIndex(const PaletteRegistry& reg,
                                    PaletteInstanceId pal,
                                    std::uint32_t c32,
                                    const QuantizePolicy& policy);

    // Palette index -> packed ImGui ABGR (opaque). Caller handles fg/bg unset semantics.
    static std::uint32_t IndexToColor32(const PaletteRegistry& reg, PaletteInstanceId pal, ColorIndex idx);
};

} // namespace phos::color


