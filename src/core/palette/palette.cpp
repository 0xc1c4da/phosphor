#include "core/palette/palette.h"

#include "core/xterm256_palette.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace phos::color
{
static inline std::uint64_t Rotl64(std::uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }

// Deterministic 128-bit-ish content hash (placeholder for BLAKE3_128; see header note).
static PaletteUid HashUidV1(std::span<const std::uint8_t> bytes)
{
    // Two-lane FNV-1a to fill 128 bits deterministically.
    std::uint64_t h0 = 1469598103934665603ull;
    std::uint64_t h1 = 1099511628211ull ^ 0x9E3779B185EBCA87ull;
    for (std::uint8_t b : bytes)
    {
        h0 ^= (std::uint64_t)b;
        h0 *= 1099511628211ull;

        h1 ^= Rotl64((std::uint64_t)b + 0xA5u, 17);
        h1 *= 14029467366897019727ull; // odd multiplier
    }

    PaletteUid out;
    // Encode little-endian.
    for (int i = 0; i < 8; ++i)
        out.bytes[(size_t)i] = (std::uint8_t)((h0 >> (i * 8)) & 0xFFu);
    for (int i = 0; i < 8; ++i)
        out.bytes[(size_t)8 + (size_t)i] = (std::uint8_t)((h1 >> (i * 8)) & 0xFFu);
    return out;
}

PaletteUid ComputePaletteUid(std::span<const Rgb8> rgb)
{
    // uid_version || count(u16le) || rgb_bytes
    const std::uint8_t version = 1;
    const std::uint16_t count = (std::uint16_t)std::min<std::size_t>(rgb.size(), kMaxPaletteSize);

    std::vector<std::uint8_t> buf;
    buf.reserve(1 + 2 + (size_t)count * 3u);
    buf.push_back(version);
    buf.push_back((std::uint8_t)(count & 0xFFu));
    buf.push_back((std::uint8_t)((count >> 8) & 0xFFu));
    for (std::uint16_t i = 0; i < count; ++i)
    {
        buf.push_back(rgb[i].r);
        buf.push_back(rgb[i].g);
        buf.push_back(rgb[i].b);
    }

    return HashUidV1(buf);
}

std::size_t PaletteRegistry::UidHash::operator()(const PaletteUid& u) const noexcept
{
    // Use the first 8 bytes as a quick hash seed (still stable).
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (std::uint64_t)u.bytes[(size_t)i] << (i * 8);
    return (std::size_t)v;
}

static std::vector<Rgb8> MakeVga16Rgb()
{
    // Match formats::ansi VGA16 (ANSI/SGR order).
    return {
        {0x00, 0x00, 0x00}, // 0 black
        {0xAA, 0x00, 0x00}, // 1 red
        {0x00, 0xAA, 0x00}, // 2 green
        {0xAA, 0x55, 0x00}, // 3 yellow/brown
        {0x00, 0x00, 0xAA}, // 4 blue
        {0xAA, 0x00, 0xAA}, // 5 magenta
        {0x00, 0xAA, 0xAA}, // 6 cyan
        {0xAA, 0xAA, 0xAA}, // 7 light gray
        {0x55, 0x55, 0x55}, // 8 dark gray
        {0xFF, 0x55, 0x55}, // 9 bright red
        {0x55, 0xFF, 0x55}, // 10 bright green
        {0xFF, 0xFF, 0x55}, // 11 bright yellow
        {0x55, 0x55, 0xFF}, // 12 bright blue
        {0xFF, 0x55, 0xFF}, // 13 bright magenta
        {0x55, 0xFF, 0xFF}, // 14 bright cyan
        {0xFF, 0xFF, 0xFF}, // 15 bright white
    };
}

static std::vector<Rgb8> MakeVga8Rgb()
{
    auto v = MakeVga16Rgb();
    v.resize(8);
    return v;
}

static std::vector<Rgb8> MakeXtermRgb(int lo, int hi)
{
    lo = std::clamp(lo, 0, 255);
    hi = std::clamp(hi, 0, 255);
    if (lo > hi)
        std::swap(lo, hi);
    std::vector<Rgb8> out;
    out.reserve((size_t)(hi - lo + 1));
    for (int i = lo; i <= hi; ++i)
    {
        const auto& c = xterm256::RgbForIndex(i);
        out.push_back({c.r, c.g, c.b});
    }
    return out;
}

PaletteRegistry::PaletteRegistry()
{
    // Register built-ins up-front (stable palette identity).
    {
        Palette p;
        p.ref.is_builtin = true;
        p.ref.builtin = BuiltinPalette::Vga8;
        p.title = "VGA 8";
        p.rgb = MakeVga8Rgb();
        // Explicit mapping to VGA16 (lossless subset).
        DerivedPaletteMapping m;
        m.parent.is_builtin = true;
        m.parent.builtin = BuiltinPalette::Vga16;
        m.derived_to_parent.resize(8);
        for (int i = 0; i < 8; ++i)
            m.derived_to_parent[(size_t)i] = (std::uint16_t)i;
        p.derived = std::move(m);
        const auto id = RegisterInternal(std::move(p));
        m_builtin_to_instance[(std::uint32_t)BuiltinPalette::Vga8] = id.v;
    }
    {
        Palette p;
        p.ref.is_builtin = true;
        p.ref.builtin = BuiltinPalette::Vga16;
        p.title = "VGA 16";
        p.rgb = MakeVga16Rgb();
        const auto id = RegisterInternal(std::move(p));
        m_builtin_to_instance[(std::uint32_t)BuiltinPalette::Vga16] = id.v;
    }
    {
        Palette p;
        p.ref.is_builtin = true;
        p.ref.builtin = BuiltinPalette::Xterm256;
        p.title = "Xterm 256";
        p.rgb = MakeXtermRgb(0, 255);
        const auto id = RegisterInternal(std::move(p));
        m_builtin_to_instance[(std::uint32_t)BuiltinPalette::Xterm256] = id.v;
    }
    {
        Palette p;
        p.ref.is_builtin = true;
        p.ref.builtin = BuiltinPalette::Xterm16;
        p.title = "Xterm 16";
        p.rgb = MakeXtermRgb(0, 15);
        // Explicit mapping to xterm256 (lossless subset).
        DerivedPaletteMapping m;
        m.parent.is_builtin = true;
        m.parent.builtin = BuiltinPalette::Xterm256;
        m.derived_to_parent.resize(16);
        for (int i = 0; i < 16; ++i)
            m.derived_to_parent[(size_t)i] = (std::uint16_t)i;
        p.derived = std::move(m);
        const auto id = RegisterInternal(std::move(p));
        m_builtin_to_instance[(std::uint32_t)BuiltinPalette::Xterm16] = id.v;
    }
    {
        Palette p;
        p.ref.is_builtin = true;
        p.ref.builtin = BuiltinPalette::Xterm240Safe;
        p.title = "Xterm 240 Safe";
        p.rgb = MakeXtermRgb(16, 255); // size 240
        // Explicit mapping to xterm256 (lossless subset/range).
        DerivedPaletteMapping m;
        m.parent.is_builtin = true;
        m.parent.builtin = BuiltinPalette::Xterm256;
        m.derived_to_parent.resize(240);
        for (int i = 0; i < 240; ++i)
            m.derived_to_parent[(size_t)i] = (std::uint16_t)(16 + i);
        p.derived = std::move(m);
        const auto id = RegisterInternal(std::move(p));
        m_builtin_to_instance[(std::uint32_t)BuiltinPalette::Xterm240Safe] = id.v;
    }
}

PaletteInstanceId PaletteRegistry::RegisterInternal(Palette p)
{
    PaletteInstanceId id;
    id.v = m_next_instance++;
    p.instance = id;
    m_by_instance[id.v] = std::move(p);
    return id;
}

std::optional<PaletteInstanceId> PaletteRegistry::Resolve(const PaletteRef& ref) const
{
    if (ref.is_builtin)
    {
        auto it = m_builtin_to_instance.find((std::uint32_t)ref.builtin);
        if (it == m_builtin_to_instance.end())
            return std::nullopt;
        return PaletteInstanceId{it->second};
    }
    if (!ref.uid.IsZero())
    {
        auto it = m_dynamic_by_uid.find(ref.uid);
        if (it == m_dynamic_by_uid.end())
            return std::nullopt;
        return PaletteInstanceId{it->second};
    }
    return std::nullopt;
}

const Palette* PaletteRegistry::Get(PaletteInstanceId id) const
{
    auto it = m_by_instance.find(id.v);
    if (it == m_by_instance.end())
        return nullptr;
    return &it->second;
}

PaletteInstanceId PaletteRegistry::Builtin(BuiltinPalette b) const
{
    auto it = m_builtin_to_instance.find((std::uint32_t)b);
    if (it == m_builtin_to_instance.end())
        return PaletteInstanceId{0};
    return PaletteInstanceId{it->second};
}

PaletteInstanceId PaletteRegistry::RegisterDynamic(std::string_view title, std::span<const Rgb8> rgb)
{
    const std::size_t n = std::min<std::size_t>(rgb.size(), kMaxPaletteSize);
    const PaletteUid uid = ComputePaletteUid(rgb.first(n));

    auto it = m_dynamic_by_uid.find(uid);
    if (it != m_dynamic_by_uid.end())
    {
        // Optionally merge/update title metadata.
        auto pit = m_by_instance.find(it->second);
        if (pit != m_by_instance.end() && pit->second.title.empty() && !title.empty())
            pit->second.title = std::string(title);
        return PaletteInstanceId{it->second};
    }

    Palette p;
    p.ref.is_builtin = false;
    p.ref.uid = uid;
    p.title = std::string(title);
    p.rgb.assign(rgb.begin(), rgb.begin() + (ptrdiff_t)n);
    const auto id = RegisterInternal(std::move(p));
    m_dynamic_by_uid[uid] = id.v;
    return id;
}

} // namespace phos::color


