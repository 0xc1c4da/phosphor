#include "core/color_ops.h"

#include "core/xterm256_palette.h"

#include <algorithm>

namespace phos::color
{
static inline int Dist2(const Rgb8& a, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const int dr = (int)a.r - (int)r;
    const int dg = (int)a.g - (int)g;
    const int db = (int)a.b - (int)b;
    return dr * dr + dg * dg + db * db;
}

std::uint8_t ColorOps::NearestIndexRgb(const PaletteRegistry& reg,
                                      PaletteInstanceId pal,
                                      std::uint8_t r,
                                      std::uint8_t g,
                                      std::uint8_t b,
                                      const QuantizePolicy& policy)
{
    const Palette* p = reg.Get(pal);
    if (!p || p->rgb.empty())
        return 0;

    // Exact fast-path for xterm256 using the existing optimized routine.
    if (p->ref.is_builtin && p->ref.builtin == BuiltinPalette::Xterm256 &&
        policy.distance == QuantizePolicy::DistanceMetric::Rgb8_SquaredEuclidean &&
        policy.tie_break_lowest_index)
    {
        return (std::uint8_t)std::clamp(xterm256::NearestIndex(r, g, b), 0, 255);
    }

    int best = 0;
    int best_d2 = 0x7fffffff;
    for (int i = 0; i < (int)p->rgb.size(); ++i)
    {
        const int d2 = Dist2(p->rgb[(size_t)i], r, g, b);
        if (d2 < best_d2)
        {
            best_d2 = d2;
            best = i;
        }
    }
    return (std::uint8_t)std::clamp(best, 0, 255);
}

ColorIndex ColorOps::Color32ToIndex(const PaletteRegistry& reg,
                                  PaletteInstanceId pal,
                                  std::uint32_t c32,
                                  const QuantizePolicy& policy)
{
    std::uint8_t r = 0, g = 0, b = 0;
    if (!UnpackImGuiAbgr(c32, r, g, b))
        return ColorIndex{ kUnsetIndex };
    return ColorIndex{ (std::uint16_t)NearestIndexRgb(reg, pal, r, g, b, policy) };
}

std::uint32_t ColorOps::IndexToColor32(const PaletteRegistry& reg, PaletteInstanceId pal, ColorIndex idx)
{
    const Palette* p = reg.Get(pal);
    if (!p || idx.IsUnset() || p->rgb.empty())
        return 0;
    const std::uint16_t i = idx.v;
    if (i >= p->rgb.size())
        return 0;
    const Rgb8 c = p->rgb[i];
    return PackImGuiAbgrOpaque(c.r, c.g, c.b);
}

} // namespace phos::color


