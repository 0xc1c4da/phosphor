#include "core/lut/lut_cache.h"

#include "core/color_blend.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace phos::color
{
static inline std::uint64_t Mix64(std::uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

std::size_t LutKeyHash::operator()(const LutKey& k) const noexcept
{
    std::uint64_t h = 0xCBF29CE484222325ULL;
    h ^= (std::uint64_t)k.type; h *= 1099511628211ULL;
    h ^= k.a.v; h *= 1099511628211ULL;
    h ^= k.b.v; h *= 1099511628211ULL;
    h ^= (std::uint64_t)k.quant_bits; h *= 1099511628211ULL;
    h ^= (std::uint64_t)k.allowed_hash; h *= 1099511628211ULL;
    h ^= (std::uint64_t)k.blend_mode; h *= 1099511628211ULL;
    h ^= (std::uint64_t)k.blend_alpha; h *= 1099511628211ULL;
    h ^= (std::uint64_t)k.quantize.distance; h *= 1099511628211ULL;
    h ^= (std::uint64_t)(k.quantize.tie_break_lowest_index ? 1 : 0); h *= 1099511628211ULL;
    return (std::size_t)Mix64(h);
}

static inline int Dist2Rgb(const Rgb8& a, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const int dr = (int)a.r - (int)r;
    const int dg = (int)a.g - (int)g;
    const int db = (int)a.b - (int)b;
    return dr * dr + dg * dg + db * db;
}

static std::uint8_t NearestIndexRgb_Scan(const Palette& pal,
                                         std::uint8_t r,
                                         std::uint8_t g,
                                         std::uint8_t b,
                                         const QuantizePolicy& policy)
{
    (void)policy; // only one metric today; tie-break = lowest index is implicit by scan order.
    // Exact reverse-map fast path (if available).
    if (!pal.exact_u24_to_index.empty())
    {
        const std::uint32_t u24 = (std::uint32_t)r | ((std::uint32_t)g << 8) | ((std::uint32_t)b << 16);
        auto it = pal.exact_u24_to_index.find(u24);
        if (it != pal.exact_u24_to_index.end())
            return it->second;
    }
    const int n = (int)pal.rgb.size();
    int best = 0;
    int best_d2 = 0x7fffffff;
    for (int i = 0; i < n; ++i)
    {
        const int d2 = Dist2Rgb(pal.rgb[(size_t)i], r, g, b);
        if (d2 < best_d2)
        {
            best_d2 = d2;
            best = i;
        }
    }
    return (std::uint8_t)std::clamp(best, 0, 255);
}

static std::uint64_t HashAllowedIndicesU8(std::span<const std::uint8_t> indices_sorted_unique)
{
    // Deterministic hash: caller provides canonicalized (sorted+unique) in-range indices.
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t x : indices_sorted_unique)
    {
        h ^= (std::uint64_t)x;
        h *= 1099511628211ull;
    }
    return Mix64(h);
}

LutCache::LutCache(std::size_t budget_bytes) : m_budget_bytes(budget_bytes) {}

void LutCache::SetBudgetBytes(std::size_t bytes)
{
    m_budget_bytes = bytes;
    EvictAsNeeded(0);
}

void LutCache::Touch(typename std::unordered_map<LutKey, Entry, LutKeyHash>::iterator it)
{
    // Move to front.
    m_lru.erase(it->second.lru_it);
    m_lru.push_front(it->first);
    it->second.lru_it = m_lru.begin();
}

void LutCache::EvictAsNeeded(std::size_t incoming_bytes)
{
    // A budget of 0 means "unlimited".
    if (m_budget_bytes == 0)
        return;
    if (incoming_bytes > m_budget_bytes)
        return; // can't fit even in an empty cache; caller must handle fallback

    while (!m_lru.empty() && m_used_bytes + incoming_bytes > m_budget_bytes)
    {
        const LutKey& k = m_lru.back();
        auto it = m_map.find(k);
        if (it != m_map.end())
        {
            m_used_bytes -= it->second.bytes;
            m_map.erase(it);
        }
        m_lru.pop_back();
    }
}

std::shared_ptr<const RgbQuantize3dLut> LutCache::GetOrBuildQuant3d(PaletteRegistry& palettes,
                                                                    PaletteInstanceId pal,
                                                                    std::uint8_t bits,
                                                                    const QuantizePolicy& policy)
{
    if (bits < 1 || bits > 6)
        return nullptr;
    const Palette* p = palettes.Get(pal);
    if (!p || p->rgb.empty() || p->rgb.size() > kMaxPaletteSize)
        return nullptr;

    LutKey key;
    key.type = LutType::Quant3D;
    key.a = pal;
    key.b = PaletteInstanceId{0};
    key.quant_bits = bits;
    key.quantize = policy;
    key.allowed_hash = 0;

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        Touch(it);
        return std::static_pointer_cast<const RgbQuantize3dLut>(it->second.payload);
    }

    const std::size_t side = (std::size_t)1u << bits;
    const std::size_t entries = side * side * side;
    const std::size_t bytes = entries * sizeof(std::uint8_t);

    EvictAsNeeded(bytes);
    if (m_budget_bytes != 0 && (bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
        return nullptr;

    auto lut = std::make_shared<RgbQuantize3dLut>();
    lut->bits = bits;
    lut->table.resize(entries);

    // Sample at bin center in 0..255:
    // bin size = 256/side, center = (bin + 0.5)*binSize.
    const int bin_size = (int)(256 / (int)side);
    for (std::size_t bz = 0; bz < side; ++bz)
    {
        const int b0 = (int)bz * bin_size;
        const int bcenter = std::min(255, b0 + bin_size / 2);
        for (std::size_t gy = 0; gy < side; ++gy)
        {
            const int g0 = (int)gy * bin_size;
            const int gcenter = std::min(255, g0 + bin_size / 2);
            for (std::size_t rx = 0; rx < side; ++rx)
            {
                const int r0 = (int)rx * bin_size;
                const int rcenter = std::min(255, r0 + bin_size / 2);

                const std::uint8_t idx = NearestIndexRgb_Scan(*p,
                                                              (std::uint8_t)rcenter,
                                                              (std::uint8_t)gcenter,
                                                              (std::uint8_t)bcenter,
                                                              policy);
                const std::size_t flat = (bz * side + gy) * side + rx;
                lut->table[flat] = idx;
            }
        }
    }

    // Insert into cache.
    m_lru.push_front(key);
    Entry e;
    e.payload = lut;
    e.bytes = bytes;
    e.lru_it = m_lru.begin();
    m_used_bytes += bytes;
    m_map.emplace(key, std::move(e));
    return lut;
}

std::shared_ptr<const RemapLut> LutCache::GetOrBuildRemap(PaletteRegistry& palettes,
                                                          PaletteInstanceId src,
                                                          PaletteInstanceId dst,
                                                          const QuantizePolicy& policy)
{
    const Palette* ps = palettes.Get(src);
    const Palette* pd = palettes.Get(dst);
    if (!ps || !pd || ps->rgb.empty() || pd->rgb.empty())
        return nullptr;
    if (ps->rgb.size() > kMaxPaletteSize || pd->rgb.size() > kMaxPaletteSize)
        return nullptr;

    LutKey key;
    key.type = LutType::Remap;
    key.a = src;
    key.b = dst;
    key.quant_bits = 0;
    key.allowed_hash = 0;
    key.quantize = policy;

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        Touch(it);
        return std::static_pointer_cast<const RemapLut>(it->second.payload);
    }

    // Fast-path: derived palette that losslessly maps to its parent (no quantization).
    // Phase C requirement: parent may be builtin or dynamic; match by resolved instance id.
    if (ps->derived && ps->derived->derived_to_parent.size() == ps->rgb.size())
    {
        const auto parent_id = palettes.Resolve(ps->derived->parent);
        if (parent_id.has_value() && *parent_id == dst)
        {
            bool ok = true;
            for (std::size_t i = 0; i < ps->rgb.size(); ++i)
            {
                const std::uint16_t pi = ps->derived->derived_to_parent[i];
                if (pi >= pd->rgb.size() || pi > 255u)
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
            {
                const std::size_t bytes = ps->rgb.size() * sizeof(std::uint8_t);
                EvictAsNeeded(bytes);
                if (m_budget_bytes == 0 || !(bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
                {
                    auto lut = std::make_shared<RemapLut>();
                    lut->remap.resize(ps->rgb.size());
                    for (std::size_t i = 0; i < ps->rgb.size(); ++i)
                        lut->remap[i] = (std::uint8_t)ps->derived->derived_to_parent[i];

                    m_lru.push_front(key);
                    Entry e;
                    e.payload = lut;
                    e.bytes = bytes;
                    e.lru_it = m_lru.begin();
                    m_used_bytes += bytes;
                    m_map.emplace(key, std::move(e));
                    return lut;
                }
                // else: budget pressure; fall through to allow caller fallback (nullptr) via normal path below.
            }
        }
    }

    const std::size_t bytes = ps->rgb.size() * sizeof(std::uint8_t);
    EvictAsNeeded(bytes);
    if (m_budget_bytes != 0 && (bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
        return nullptr;

    auto lut = std::make_shared<RemapLut>();
    lut->remap.resize(ps->rgb.size());
    for (std::size_t i = 0; i < ps->rgb.size(); ++i)
    {
        const Rgb8 c = ps->rgb[i];
        lut->remap[i] = NearestIndexRgb_Scan(*pd, c.r, c.g, c.b, policy);
    }

    m_lru.push_front(key);
    Entry e;
    e.payload = lut;
    e.bytes = bytes;
    e.lru_it = m_lru.begin();
    m_used_bytes += bytes;
    m_map.emplace(key, std::move(e));
    return lut;
}

std::shared_ptr<const AllowedRgbQuantize3dLut> LutCache::GetOrBuildAllowedQuant3d(PaletteRegistry& palettes,
                                                                                  PaletteInstanceId pal,
                                                                                  std::span<const int> allowed_indices,
                                                                                  std::uint8_t bits,
                                                                                  const QuantizePolicy& policy)
{
    if (bits < 1 || bits > 6)
        return nullptr;
    const Palette* p = palettes.Get(pal);
    if (!p || p->rgb.empty() || p->rgb.size() > kMaxPaletteSize)
        return nullptr;
    if (allowed_indices.empty())
        return nullptr;

    // Normalize allowed list to unique in-range u8 indices.
    std::vector<std::uint8_t> allowed;
    allowed.reserve(allowed_indices.size());
    for (int idx : allowed_indices)
    {
        if (idx < 0 || idx >= (int)p->rgb.size())
            continue;
        allowed.push_back((std::uint8_t)idx);
    }
    if (allowed.empty())
        return nullptr;
    std::sort(allowed.begin(), allowed.end());
    allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());

    LutKey key;
    key.type = LutType::AllowedQuant3D;
    key.a = pal;
    key.b = PaletteInstanceId{0};
    key.quant_bits = bits;
    key.allowed_hash = HashAllowedIndicesU8(allowed);
    key.quantize = policy;

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        Touch(it);
        return std::static_pointer_cast<const AllowedRgbQuantize3dLut>(it->second.payload);
    }

    const std::size_t side = (std::size_t)1u << bits;
    const std::size_t entries = side * side * side;
    const std::size_t bytes = entries * sizeof(std::uint8_t);

    EvictAsNeeded(bytes);
    if (m_budget_bytes != 0 && (bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
        return nullptr;

    auto lut = std::make_shared<AllowedRgbQuantize3dLut>();
    lut->bits = bits;
    lut->table.resize(entries);

    const int bin_size = (int)(256 / (int)side);
    for (std::size_t bz = 0; bz < side; ++bz)
    {
        const int b0 = (int)bz * bin_size;
        const int bcenter = std::min(255, b0 + bin_size / 2);
        for (std::size_t gy = 0; gy < side; ++gy)
        {
            const int g0 = (int)gy * bin_size;
            const int gcenter = std::min(255, g0 + bin_size / 2);
            for (std::size_t rx = 0; rx < side; ++rx)
            {
                const int r0 = (int)rx * bin_size;
                const int rcenter = std::min(255, r0 + bin_size / 2);

                int best_d2 = 0x7fffffff;
                std::uint8_t best_idx = allowed[0];
                for (std::uint8_t ai : allowed)
                {
                    const int d2 = Dist2Rgb(p->rgb[(size_t)ai],
                                            (std::uint8_t)rcenter,
                                            (std::uint8_t)gcenter,
                                            (std::uint8_t)bcenter);
                    if (d2 < best_d2)
                    {
                        best_d2 = d2;
                        best_idx = ai;
                    }
                }

                const std::size_t flat = (bz * side + gy) * side + rx;
                lut->table[flat] = best_idx;
            }
        }
    }

    m_lru.push_front(key);
    Entry e;
    e.payload = lut;
    e.bytes = bytes;
    e.lru_it = m_lru.begin();
    m_used_bytes += bytes;
    m_map.emplace(key, std::move(e));
    return lut;
}

std::shared_ptr<const AllowedSnapLut> LutCache::GetOrBuildAllowedSnap(PaletteRegistry& palettes,
                                                                      PaletteInstanceId pal,
                                                                      std::span<const int> allowed_indices,
                                                                      const QuantizePolicy& policy)
{
    const Palette* p = palettes.Get(pal);
    if (!p || p->rgb.empty() || p->rgb.size() > kMaxPaletteSize)
        return nullptr;
    if (allowed_indices.empty())
        return nullptr;

    // Normalize allowed list to unique in-range u8 indices.
    std::vector<std::uint8_t> allowed;
    allowed.reserve(allowed_indices.size());
    for (int idx : allowed_indices)
    {
        if (idx < 0 || idx >= (int)p->rgb.size())
            continue;
        allowed.push_back((std::uint8_t)idx);
    }
    if (allowed.empty())
        return nullptr;
    std::sort(allowed.begin(), allowed.end());
    allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());

    LutKey key;
    key.type = LutType::AllowedSnap;
    key.a = pal;
    key.b = PaletteInstanceId{0};
    key.quant_bits = 0;
    key.allowed_hash = HashAllowedIndicesU8(allowed);
    key.quantize = policy;

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        Touch(it);
        return std::static_pointer_cast<const AllowedSnapLut>(it->second.payload);
    }

    const std::size_t bytes = p->rgb.size() * sizeof(std::uint8_t);
    EvictAsNeeded(bytes);
    if (m_budget_bytes != 0 && (bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
        return nullptr;

    auto lut = std::make_shared<AllowedSnapLut>();
    lut->snap.resize(p->rgb.size());
    for (std::size_t i = 0; i < p->rgb.size(); ++i)
    {
        const Rgb8 c = p->rgb[i];
        int best_d2 = 0x7fffffff;
        std::uint8_t best_idx = allowed[0];
        for (std::uint8_t ai : allowed)
        {
            const int d2 = Dist2Rgb(p->rgb[(size_t)ai], c.r, c.g, c.b);
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best_idx = ai;
            }
        }
        lut->snap[i] = best_idx;
    }

    m_lru.push_front(key);
    Entry e;
    e.payload = lut;
    e.bytes = bytes;
    e.lru_it = m_lru.begin();
    m_used_bytes += bytes;
    m_map.emplace(key, std::move(e));
    return lut;
}

std::shared_ptr<const BlendLut> LutCache::GetOrBuildBlend(PaletteRegistry& palettes,
                                                          PaletteInstanceId pal,
                                                          phos::LayerBlendMode mode,
                                                          std::uint8_t alpha,
                                                          const QuantizePolicy& policy)
{
    const Palette* p = palettes.Get(pal);
    if (!p || p->rgb.empty() || p->rgb.size() > kMaxPaletteSize)
        return nullptr;

    const std::size_t n = p->rgb.size();
    if (n < 1 || n > 256)
        return nullptr;

    LutKey key;
    key.type = LutType::Blend;
    key.a = pal;
    key.b = PaletteInstanceId{0};
    key.quant_bits = 0;
    key.allowed_hash = 0;
    key.quantize = policy;
    key.blend_mode = (std::uint8_t)mode;
    key.blend_alpha = alpha;

    auto it = m_map.find(key);
    if (it != m_map.end())
    {
        Touch(it);
        return std::static_pointer_cast<const BlendLut>(it->second.payload);
    }

    const std::size_t entries = n * n;
    const std::size_t bytes = entries * sizeof(std::uint8_t);

    EvictAsNeeded(bytes);
    if (m_budget_bytes != 0 && (bytes > m_budget_bytes || m_used_bytes + bytes > m_budget_bytes))
        return nullptr;

    auto lut = std::make_shared<BlendLut>();
    lut->pal_size = (std::uint16_t)std::clamp<std::size_t>(n, 0, 256);
    lut->mode = mode;
    lut->alpha = alpha;
    lut->table.resize(entries);

    // Build: for each (base, src) -> out index.
    // Special cases are encoded to avoid unnecessary quantization scans.
    for (std::size_t bi = 0; bi < n; ++bi)
    {
        for (std::size_t si = 0; si < n; ++si)
        {
            std::uint8_t out = 0;
            if (alpha == 0)
            {
                out = (std::uint8_t)bi;
            }
            else if (mode == phos::LayerBlendMode::Normal && alpha == 255)
            {
                out = (std::uint8_t)si;
            }
            else
            {
                const Rgb8 base_rgb = p->rgb[bi];
                const Rgb8 src_rgb = p->rgb[si];
                const Rgb8 res_rgb = phos::color::BlendOverRgb(base_rgb, src_rgb, mode, alpha);
                out = NearestIndexRgb_Scan(*p, res_rgb.r, res_rgb.g, res_rgb.b, policy);
            }
            lut->table[bi * n + si] = out;
        }
    }

    m_lru.push_front(key);
    Entry e;
    e.payload = lut;
    e.bytes = bytes;
    e.lru_it = m_lru.begin();
    m_used_bytes += bytes;
    m_map.emplace(key, std::move(e));
    return lut;
}

} // namespace phos::color


