#include "blake3_util.h"

#include <blake3.h>

namespace phos::p2p
{
std::array<std::uint8_t, 32> Blake3_32(std::span<const std::uint8_t> data)
{
    std::array<std::uint8_t, 32> out{};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data.data(), data.size());
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    return out;
}

std::array<std::uint8_t, 32> Blake3_32(std::string_view s)
{
    return Blake3_32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}
} // namespace phos::p2p


