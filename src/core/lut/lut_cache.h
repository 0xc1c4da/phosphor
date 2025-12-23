#pragma once

#include "core/palette/palette.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace phos::color
{
enum class LutType : std::uint8_t
{
    Quant3D = 1,
    Remap = 2,
    AllowedQuant3D = 3,
    AllowedSnap = 4,
};

struct LutKey
{
    LutType type = LutType::Quant3D;
    PaletteInstanceId a; // palette instance (or src)
    PaletteInstanceId b; // palette instance (or dst)
    QuantizePolicy quantize;
    std::uint8_t quant_bits = 0; // 0 => invalid
    std::uint64_t allowed_hash = 0; // for AllowedQuant3D

    bool operator==(const LutKey& o) const
    {
        return type == o.type &&
               a == o.a &&
               b == o.b &&
               quant_bits == o.quant_bits &&
               allowed_hash == o.allowed_hash &&
               quantize.distance == o.quantize.distance &&
               quantize.tie_break_lowest_index == o.quantize.tie_break_lowest_index;
    }
};

struct LutKeyHash
{
    std::size_t operator()(const LutKey& k) const noexcept;
};

class RgbQuantize3dLut
{
public:
    // Table is (1<<bits)^3 entries, each is palette index (0..paletteSize-1) stored as u8.
    std::uint8_t bits = 0;
    std::vector<std::uint8_t> table;
};

class RemapLut
{
public:
    // remap[srcIndex] -> dstIndex
    std::vector<std::uint8_t> remap;
};

class AllowedRgbQuantize3dLut
{
public:
    // Table is (1<<bits)^3 entries, each is a palette index (0..paletteSize-1) stored as u8.
    // Semantics: quantize RGB directly to the nearest entry among an allowed subset.
    // The returned value is a palette index in the *full palette* (e.g. xterm index).
    std::uint8_t bits = 0;
    std::vector<std::uint8_t> table;
};

class AllowedSnapLut
{
public:
    // snap[fullIndex] -> nearest allowed palette index (both are in the full palette index space).
    std::vector<std::uint8_t> snap;
};

class LutCache
{
public:
    explicit LutCache(std::size_t budget_bytes = 64u * 1024u * 1024u);

    void SetBudgetBytes(std::size_t bytes);
    std::size_t BudgetBytes() const { return m_budget_bytes; }
    std::size_t UsedBytes() const { return m_used_bytes; }

    std::shared_ptr<const RgbQuantize3dLut> GetOrBuildQuant3d(PaletteRegistry& palettes,
                                                              PaletteInstanceId pal,
                                                              std::uint8_t bits,
                                                              const QuantizePolicy& policy);

    std::shared_ptr<const RemapLut> GetOrBuildRemap(PaletteRegistry& palettes,
                                                    PaletteInstanceId src,
                                                    PaletteInstanceId dst,
                                                    const QuantizePolicy& policy);

    std::shared_ptr<const AllowedRgbQuantize3dLut> GetOrBuildAllowedQuant3d(PaletteRegistry& palettes,
                                                                            PaletteInstanceId pal,
                                                                            std::span<const int> allowed_indices,
                                                                            std::uint8_t bits,
                                                                            const QuantizePolicy& policy);

    std::shared_ptr<const AllowedSnapLut> GetOrBuildAllowedSnap(PaletteRegistry& palettes,
                                                                PaletteInstanceId pal,
                                                                std::span<const int> allowed_indices,
                                                                const QuantizePolicy& policy);

private:
    struct Entry
    {
        std::shared_ptr<void> payload;
        std::size_t bytes = 0;
        std::list<LutKey>::iterator lru_it;
    };

    void Touch(typename std::unordered_map<LutKey, Entry, LutKeyHash>::iterator it);
    void EvictAsNeeded(std::size_t incoming_bytes);

    std::size_t m_budget_bytes = 0;
    std::size_t m_used_bytes = 0;
    std::list<LutKey> m_lru; // front = most recent
    std::unordered_map<LutKey, Entry, LutKeyHash> m_map;
};

} // namespace phos::color


