// Shared xterm-256 palette implementation.
//
// The palette layout matches the widely-used xterm-256 definition:
// - 0..15   : ANSI base colors
// - 16..231 : 6x6x6 color cube (levels: 0,95,135,175,215,255)
// - 232..255: grayscale ramp (24 steps, 8..238)
//
// NearestIndex() uses the canonical cube/grayscale projection and picks the best
// of those candidates (plus the first 16 entries) by comparing squared distance.

#include "xterm256_palette.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace xterm256
{
namespace
{
constexpr std::uint32_t PackImGuiCol32(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Matches Dear ImGui's IM_COL32(R,G,B,A) packing (A in high byte, then B,G,R).
    return 0xFF000000u | ((std::uint32_t)b << 16) | ((std::uint32_t)g << 8) | (std::uint32_t)r;
}

constexpr std::array<Rgb, 256> BuildPalette()
{
    std::array<Rgb, 256> p{};

    auto set = [&](int idx, int r, int g, int b) {
        p[(size_t)idx] = Rgb{(std::uint8_t)r, (std::uint8_t)g, (std::uint8_t)b};
    };

    // 0–15: standard ANSI colors (common xterm defaults)
    set(0, 0, 0, 0);
    set(1, 205, 0, 0);
    set(2, 0, 205, 0);
    set(3, 205, 205, 0);
    set(4, 0, 0, 238);
    set(5, 205, 0, 205);
    set(6, 0, 205, 205);
    set(7, 229, 229, 229);
    set(8, 127, 127, 127);
    set(9, 255, 0, 0);
    set(10, 0, 255, 0);
    set(11, 255, 255, 0);
    set(12, 92, 92, 255);
    set(13, 255, 0, 255);
    set(14, 0, 255, 255);
    set(15, 255, 255, 255);

    // 16–231: 6x6x6 color cube
    constexpr int level[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 16; i <= 231; ++i)
    {
        int idx = i - 16;
        int rr = idx / 36;
        int gg = (idx % 36) / 6;
        int bb = idx % 6;
        set(i, level[rr], level[gg], level[bb]);
    }

    // 232–255: grayscale ramp
    for (int i = 232; i <= 255; ++i)
    {
        int shade = 8 + (i - 232) * 10;
        set(i, shade, shade, shade);
    }

    return p;
}

constexpr std::array<Rgb, 256> kPalette = BuildPalette();

static inline int Dist2(std::uint8_t r0, std::uint8_t g0, std::uint8_t b0,
                        std::uint8_t r1, std::uint8_t g1, std::uint8_t b1)
{
    const int dr = (int)r0 - (int)r1;
    const int dg = (int)g0 - (int)g1;
    const int db = (int)b0 - (int)b1;
    return dr * dr + dg * dg + db * db;
}

static inline int NearestLevelIndex(std::uint8_t v)
{
    // Nearest index in [0,95,135,175,215,255].
    // Branchy but tiny and fast.
    if (v < 48)  return 0;        // closer to 0
    if (v < 115) return 1;        // 95
    if (v < 155) return 2;        // 135
    if (v < 195) return 3;        // 175
    if (v < 235) return 4;        // 215
    return 5;                     // 255
}

static inline std::uint8_t LevelValue(int idx)
{
    constexpr std::uint8_t level[6] = {0, 95, 135, 175, 215, 255};
    return level[(size_t)idx];
}
} // namespace

const Rgb& RgbForIndex(int idx)
{
    const std::uint8_t i = ClampIndex(idx);
    return kPalette[(size_t)i];
}

std::uint32_t Color32ForIndex(int idx)
{
    const Rgb& c = RgbForIndex(idx);
    return PackImGuiCol32(c.r, c.g, c.b);
}

int NearestIndex(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Candidate 1: 6x6x6 cube
    const int ri = NearestLevelIndex(r);
    const int gi = NearestLevelIndex(g);
    const int bi = NearestLevelIndex(b);
    const std::uint8_t cr = LevelValue(ri);
    const std::uint8_t cg = LevelValue(gi);
    const std::uint8_t cb = LevelValue(bi);
    const int cube_idx = 16 + 36 * ri + 6 * gi + bi;
    int best_idx = cube_idx;
    int best_d2  = Dist2(r, g, b, cr, cg, cb);

    // Candidate 2: grayscale ramp
    const int gray = (int)r + (int)g + (int)b;
    // Convert to approximate average in 0..255 (avoid float): round(gray/3).
    const int avg = (gray + 1) / 3;
    int gray_idx = 232;
    // Grayscale entries are: 8 + 10*k for k=0..23.
    // Find nearest k to (avg-8)/10.
    if (avg <= 8) gray_idx = 232;
    else if (avg >= 238) gray_idx = 255;
    else
    {
        const int k = (avg - 8 + 5) / 10; // rounded
        gray_idx = 232 + std::clamp(k, 0, 23);
    }
    const Rgb& gr = kPalette[(size_t)gray_idx];
    const int gray_d2 = Dist2(r, g, b, gr.r, gr.g, gr.b);
    if (gray_d2 < best_d2)
    {
        best_d2 = gray_d2;
        best_idx = gray_idx;
    }

    // Candidate 3: base 16 ANSI colors (some inputs map nicer here)
    // Only 16 checks, cheap, improves fidelity for many "named" colors.
    for (int i = 0; i < 16; ++i)
    {
        const Rgb& p = kPalette[(size_t)i];
        const int d2 = Dist2(r, g, b, p.r, p.g, p.b);
        if (d2 < best_d2)
        {
            best_d2 = d2;
            best_idx = i;
        }
    }

    return best_idx;
}
} // namespace xterm256


