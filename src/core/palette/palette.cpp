#include "core/palette/palette.h"

#include "core/xterm256_palette.h"

#include <algorithm>
#include <blake3.h>
#include <cstring>
#include <span>

namespace phos::colour
{
QuantizePolicy DefaultQuantizePolicy()
{
    QuantizePolicy p;
    p.distance = QuantizePolicy::DistanceMetric::Rgb8_SquaredEuclidean;
    p.tie_break_lowest_index = true;
    return p;
}

static PaletteUid HashUid_Blake3_128(std::span<const std::uint8_t> bytes)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    if (!bytes.empty())
        blake3_hasher_update(&hasher, bytes.data(), bytes.size());

    PaletteUid out;
    blake3_hasher_finalize(&hasher, out.bytes.data(), out.bytes.size());
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

    return HashUid_Blake3_128(buf);
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

static bool ValidateDerivedMapping(const Palette& derived, const Palette& parent)
{
    if (!derived.derived)
        return true;
    const auto& m = *derived.derived;
    if (derived.rgb.size() != m.derived_to_parent.size())
        return false;
    if (derived.rgb.empty())
        return false;
    if (parent.rgb.empty())
        return false;
    if (parent.rgb.size() > kMaxPaletteSize || derived.rgb.size() > kMaxPaletteSize)
        return false;

    for (std::size_t i = 0; i < derived.rgb.size(); ++i)
    {
        const std::uint16_t pi = m.derived_to_parent[i];
        if (pi >= parent.rgb.size())
            return false;
        const Rgb8 a = derived.rgb[i];
        const Rgb8 b = parent.rgb[(size_t)pi];
        if (a.r != b.r || a.g != b.g || a.b != b.b)
            return false;
    }
    return true;
}

PaletteRegistry::PaletteRegistry()
{
    // Register built-ins up-front (stable palette identity).
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
    // Validate derived palette mapping at registration time (Phase C requirement).
    if (p.derived)
    {
        const auto parent_id = Resolve(p.derived->parent);
        const Palette* parent = nullptr;
        if (parent_id.has_value())
        {
            auto it = m_by_instance.find(parent_id->v);
            if (it != m_by_instance.end())
                parent = &it->second;
        }
        if (!parent || !ValidateDerivedMapping(p, *parent))
        {
            // If invalid (or parent not registered), treat it as a non-derived palette.
            // This keeps the palette usable while preventing unsafe fast paths.
            p.derived.reset();
        }
    }

    PaletteInstanceId id;
    id.v = m_next_instance++;
    p.instance = id;

    // Build exact reverse lookup map (RGB24 -> lowest index).
    // This is used as a fast path for "already-in-palette" colours and for deterministic blending outputs.
    p.exact_u24_to_index.clear();
    p.exact_u24_to_index.reserve(p.rgb.size());
    for (std::size_t i = 0; i < p.rgb.size(); ++i)
    {
        const Rgb8 c = p.rgb[i];
        const std::uint32_t u24 = (std::uint32_t)c.r | ((std::uint32_t)c.g << 8) | ((std::uint32_t)c.b << 16);
        if (u24 > 0xFFFFFFu)
            continue;
        auto it = p.exact_u24_to_index.find(u24);
        if (it == p.exact_u24_to_index.end())
            p.exact_u24_to_index.emplace(u24, (std::uint8_t)std::clamp<std::size_t>(i, 0, 255));
        // else: keep existing (lowest index wins)
    }

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

} // namespace phos::colour


