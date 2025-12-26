#include "core/colour_ops.h"

#include "core/xterm256_palette.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace phos::colour
{
namespace
{
struct NearestMemoKey
{
    std::uint64_t pal = 0;
    std::uint32_t u24 = 0; // r | g<<8 | b<<16
    std::uint8_t  metric = 0;
    std::uint8_t  tie_break_lowest = 0;

    bool operator==(const NearestMemoKey& o) const
    {
        return pal == o.pal &&
               u24 == o.u24 &&
               metric == o.metric &&
               tie_break_lowest == o.tie_break_lowest;
    }
};

struct NearestMemoKeyHash
{
    std::size_t operator()(const NearestMemoKey& k) const noexcept
    {
        // Simple mix (good enough for a small bounded cache).
        std::uint64_t h = 1469598103934665603ull;
        auto mix_u64 = [&](std::uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix_u64(k.pal);
        mix_u64((std::uint64_t)k.u24);
        mix_u64((std::uint64_t)k.metric);
        mix_u64((std::uint64_t)k.tie_break_lowest);
        // Final avalanche-ish
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return (std::size_t)h;
    }
};

struct NearestMemoEntry
{
    std::uint8_t out = 0;
    std::list<NearestMemoKey>::iterator lru_it;
};

// Thread-local bounded LRU cache:
// - Avoids locking.
// - Keeps behavior deterministic (cache hit/miss does not change the computed result).
// - Big win for UI sliders and scripts that quantize the same RGBs repeatedly.
struct NearestMemoCache
{
    static constexpr std::size_t kMaxEntries = 16384;

    std::list<NearestMemoKey> lru; // front = most recent
    std::unordered_map<NearestMemoKey, NearestMemoEntry, NearestMemoKeyHash> map;

    bool Get(const NearestMemoKey& k, std::uint8_t& out)
    {
        auto it = map.find(k);
        if (it == map.end())
            return false;
        // Touch
        lru.erase(it->second.lru_it);
        lru.push_front(k);
        it->second.lru_it = lru.begin();
        out = it->second.out;
        return true;
    }

    void Put(const NearestMemoKey& k, std::uint8_t out)
    {
        // Update existing
        auto it = map.find(k);
        if (it != map.end())
        {
            it->second.out = out;
            lru.erase(it->second.lru_it);
            lru.push_front(k);
            it->second.lru_it = lru.begin();
            return;
        }

        // Evict if full
        if (map.size() >= kMaxEntries && !lru.empty())
        {
            const NearestMemoKey& old = lru.back();
            auto oit = map.find(old);
            if (oit != map.end())
                map.erase(oit);
            lru.pop_back();
        }

        lru.push_front(k);
        NearestMemoEntry e;
        e.out = out;
        e.lru_it = lru.begin();
        map.emplace(k, e);
    }
};

static thread_local NearestMemoCache g_nearest_memo;
} // namespace

static inline int Dist2(const Rgb8& a, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const int dr = (int)a.r - (int)r;
    const int dg = (int)a.g - (int)g;
    const int db = (int)a.b - (int)b;
    return dr * dr + dg * dg + db * db;
}

std::uint8_t ColourOps::NearestIndexRgb(const PaletteRegistry& reg,
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

    // Exact-match fast path for all palettes (including dynamic palettes).
    const std::uint32_t u24 = (std::uint32_t)r | ((std::uint32_t)g << 8) | ((std::uint32_t)b << 16);
    if (!p->exact_u24_to_index.empty())
    {
        auto it = p->exact_u24_to_index.find(u24);
        if (it != p->exact_u24_to_index.end())
            return it->second;
    }

    // Nearest memo cache fast-path (bounded, per-thread).
    {
        NearestMemoKey k;
        k.pal = pal.v;
        k.u24 = u24;
        k.metric = (std::uint8_t)policy.distance;
        k.tie_break_lowest = policy.tie_break_lowest_index ? 1u : 0u;
        std::uint8_t cached = 0;
        if (g_nearest_memo.Get(k, cached))
            return cached;
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
    const std::uint8_t out = (std::uint8_t)std::clamp(best, 0, 255);
    {
        NearestMemoKey k;
        k.pal = pal.v;
        k.u24 = u24;
        k.metric = (std::uint8_t)policy.distance;
        k.tie_break_lowest = policy.tie_break_lowest_index ? 1u : 0u;
        g_nearest_memo.Put(k, out);
    }
    return out;
}

ColourIndex ColourOps::Colour32ToIndex(const PaletteRegistry& reg,
                                  PaletteInstanceId pal,
                                  std::uint32_t c32,
                                  const QuantizePolicy& policy)
{
    std::uint8_t r = 0, g = 0, b = 0;
    if (!UnpackImGuiAbgr(c32, r, g, b))
        return ColourIndex{ kUnsetIndex };
    return ColourIndex{ (std::uint16_t)NearestIndexRgb(reg, pal, r, g, b, policy) };
}

std::uint32_t ColourOps::IndexToColour32(const PaletteRegistry& reg, PaletteInstanceId pal, ColourIndex idx)
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

} // namespace phos::colour


