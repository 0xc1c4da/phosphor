#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phos::color
{
// Hard cap for palettes in the LUT-centric model (see references/luts-refactor.md).
static constexpr std::uint16_t kMaxPaletteSize = 256;

struct Rgb8
{
    std::uint8_t r = 0, g = 0, b = 0;
};

enum class BuiltinPalette : std::uint32_t
{
    None = 0,
    Vga16 = 1,
    Xterm256 = 2,
    Xterm16 = 3,
    Xterm240Safe = 4, // xterm indices 16..255 (240 colors)
    Vga8 = 5,
};

struct PaletteUid
{
    // NOTE: Spec calls for BLAKE3_128. We start with a deterministic content hash and keep a
    // version byte so we can swap to BLAKE3 later without ambiguity.
    std::array<std::uint8_t, 16> bytes = {};

    bool operator==(const PaletteUid& o) const { return bytes == o.bytes; }
    bool operator!=(const PaletteUid& o) const { return !(*this == o); }
    bool IsZero() const
    {
        for (std::uint8_t b : bytes)
            if (b != 0)
                return false;
        return true;
    }
};

struct PaletteRef
{
    bool is_builtin = false;
    BuiltinPalette builtin = BuiltinPalette::None;
    PaletteUid uid; // used when !is_builtin
};

struct PaletteInstanceId
{
    std::uint64_t v = 0;
    bool operator==(const PaletteInstanceId& o) const { return v == o.v; }
    bool operator!=(const PaletteInstanceId& o) const { return v != o.v; }
};

struct DerivedPaletteMapping
{
    PaletteRef parent;
    // derived_to_parent[i] gives the parent palette index for derived index i.
    std::vector<std::uint16_t> derived_to_parent;
};

struct Palette
{
    PaletteRef ref;
    PaletteInstanceId instance;
    std::string title;
    std::vector<Rgb8> rgb; // size 1..256
    std::optional<DerivedPaletteMapping> derived;
};

struct QuantizePolicy
{
    enum class DistanceMetric : std::uint8_t
    {
        Rgb8_SquaredEuclidean = 1,
    };

    DistanceMetric distance = DistanceMetric::Rgb8_SquaredEuclidean;
    bool tie_break_lowest_index = true;
};

// Phase C: authoritative default quantization policy used across the codebase.
// Locked defaults (see references/phase-c-refactor.md):
// - Distance metric: Rgb8_SquaredEuclidean
// - Tie-break: lowest index
QuantizePolicy DefaultQuantizePolicy();

PaletteUid ComputePaletteUid(std::span<const Rgb8> rgb);

class PaletteRegistry
{
public:
    PaletteRegistry();

    // Returns nullopt if not found.
    std::optional<PaletteInstanceId> Resolve(const PaletteRef& ref) const;
    const Palette* Get(PaletteInstanceId id) const;

    // Built-ins are registered at construction and always resolve.
    PaletteInstanceId Builtin(BuiltinPalette b) const;

    // Register or reuse (intern) a dynamic palette by content-addressed uid.
    // Returns the interned instance id.
    PaletteInstanceId RegisterDynamic(std::string_view title, std::span<const Rgb8> rgb);

private:
    PaletteInstanceId RegisterInternal(Palette p);

    struct UidHash
    {
        std::size_t operator()(const PaletteUid& u) const noexcept;
    };

    std::unordered_map<std::uint64_t, Palette> m_by_instance;
    std::unordered_map<PaletteUid, std::uint64_t, UidHash> m_dynamic_by_uid;
    std::unordered_map<std::uint32_t, std::uint64_t> m_builtin_to_instance;
    std::uint64_t m_next_instance = 1;
};

} // namespace phos::color


