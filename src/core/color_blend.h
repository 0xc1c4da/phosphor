#pragma once

#include "core/layer_blend_mode.h"
#include "core/palette/palette.h"

#include <algorithm>
#include <cstdint>

namespace phos::color
{
// Shared blend helpers (deterministic integer math; matches references/luts-refactor.md Phase D v1 formulas).
static inline std::uint8_t LerpU8(std::uint8_t a, std::uint8_t b, std::uint8_t t)
{
    // Round-to-nearest lerp in 8-bit space: (a*(255-t) + b*t)/255.
    const std::uint32_t v =
        (std::uint32_t)a * (255u - (std::uint32_t)t) +
        (std::uint32_t)b * (std::uint32_t)t;
    return (std::uint8_t)((v + 127u) / 255u);
}

static inline std::uint8_t Mul255(std::uint32_t x, std::uint32_t y)
{
    return (std::uint8_t)((x * y + 127u) / 255u);
}

static inline std::uint32_t DivRound(std::uint32_t num, std::uint32_t den)
{
    if (den == 0)
        return 0;
    return (num + (den / 2u)) / den;
}

static inline std::uint8_t BlendChannel(std::uint8_t b, std::uint8_t s, phos::LayerBlendMode mode)
{
    switch (mode)
    {
        case phos::LayerBlendMode::Normal:
            return s;
        case phos::LayerBlendMode::Multiply:
            return Mul255(b, s);
        case phos::LayerBlendMode::Screen:
            return (std::uint8_t)(255u - (std::uint32_t)Mul255(255u - b, 255u - s));
        case phos::LayerBlendMode::Overlay:
            if (b <= 127u)
                return (std::uint8_t)((2u * (std::uint32_t)b * (std::uint32_t)s + 127u) / 255u);
            return (std::uint8_t)(255u - (std::uint32_t)((2u * (255u - (std::uint32_t)b) * (255u - (std::uint32_t)s) + 127u) / 255u));
        case phos::LayerBlendMode::Darken:
            return (b < s) ? b : s;
        case phos::LayerBlendMode::Lighten:
            return (b > s) ? b : s;
        case phos::LayerBlendMode::ColorDodge:
            if (s == 255u)
                return 255u;
            return (std::uint8_t)std::min<std::uint32_t>(255u, DivRound((std::uint32_t)b * 255u, 255u - (std::uint32_t)s));
        case phos::LayerBlendMode::ColorBurn:
            if (s == 0u)
                return 0u;
            return (std::uint8_t)(255u - std::min<std::uint32_t>(255u, DivRound((255u - (std::uint32_t)b) * 255u, (std::uint32_t)s)));
    }
    return s;
}

static inline Rgb8 BlendRgb(const Rgb8& base, const Rgb8& src, phos::LayerBlendMode mode)
{
    Rgb8 out;
    out.r = BlendChannel(base.r, src.r, mode);
    out.g = BlendChannel(base.g, src.g, mode);
    out.b = BlendChannel(base.b, src.b, mode);
    return out;
}

static inline Rgb8 ApplyOpacityRgb(const Rgb8& base, const Rgb8& blended, std::uint8_t alpha)
{
    if (alpha == 255)
        return blended;
    if (alpha == 0)
        return base;
    return Rgb8{
        LerpU8(base.r, blended.r, alpha),
        LerpU8(base.g, blended.g, alpha),
        LerpU8(base.b, blended.b, alpha),
    };
}

static inline Rgb8 BlendOverRgb(const Rgb8& base, const Rgb8& src, phos::LayerBlendMode mode, std::uint8_t alpha)
{
    return ApplyOpacityRgb(base, BlendRgb(base, src, mode), alpha);
}
} // namespace phos::color


