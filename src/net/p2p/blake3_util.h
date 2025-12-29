// Blake3 helpers for P2P tags/ids.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace phos::p2p
{
std::array<std::uint8_t, 32> Blake3_32(std::span<const std::uint8_t> data);
std::array<std::uint8_t, 32> Blake3_32(std::string_view s);

inline std::uint64_t TagU64FromFirst8LE(const std::array<std::uint8_t, 32>& h)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (static_cast<std::uint64_t>(h[static_cast<std::size_t>(i)]) << (8 * i));
    return v;
}
} // namespace phos::p2p


