#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace deform
{
// Cached per-glyph coverage masks at a specific cell pixel resolution (scale already applied).
// Masks are stored as 8-bit alpha (0..255), row-major, size = w*h.
class GlyphMaskCache
{
public:
    struct Mask
    {
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> a; // length w*h
    };

    // Returns a mask for `glyph` at the given `cell_w_px` x `cell_h_px` pixel resolution.
    // The mask generation matches `canvas_rasterizer` glyph placement and bitmap font rules.
    //
    // On failure/unavailable glyph, returns an empty/zero mask (w/h still set, data filled with 0).
    Mask GetMask(const AnsiCanvas& canvas,
                 int cell_w_px,
                 int cell_h_px,
                 int scale,
                 AnsiCanvas::GlyphId glyph,
                 std::string& err);

    void Clear() { cache_.clear(); }

private:
    struct Key
    {
        std::uint64_t font_key = 0;
        int cell_w_px = 0;
        int cell_h_px = 0;
        int scale = 1;
        std::uint32_t glyph = 0;

        bool operator==(const Key& o) const
        {
            return font_key == o.font_key &&
                   cell_w_px == o.cell_w_px &&
                   cell_h_px == o.cell_h_px &&
                   scale == o.scale &&
                   glyph == o.glyph;
        }
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& k) const noexcept
        {
            // Simple 64-bit mix.
            std::uint64_t x = k.font_key;
            x ^= (std::uint64_t)(std::uint32_t)k.cell_w_px + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2);
            x ^= (std::uint64_t)(std::uint32_t)k.cell_h_px + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2);
            x ^= (std::uint64_t)(std::uint32_t)k.scale + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2);
            x ^= (std::uint64_t)k.glyph + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2);
            return (std::size_t)x;
        }
    };

    std::unordered_map<Key, Mask, KeyHash> cache_;
};
} // namespace deform


