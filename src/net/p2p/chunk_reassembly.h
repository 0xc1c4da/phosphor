// Chunk reassembly for PHOS_F_CHUNKED transfers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace phos::p2p
{
struct ChunkCaps
{
    std::size_t max_inflight_transfers = 32;
    std::size_t max_chunks_per_transfer = 512;
    std::size_t max_total_bytes_per_transfer = 16 * 1024 * 1024; // 16 MiB
};

struct TransferKey
{
    std::uint64_t from_tag = 0;
    std::array<std::uint8_t, 16> xfer_id{};

    bool operator==(const TransferKey& o) const { return from_tag == o.from_tag && xfer_id == o.xfer_id; }
};

struct TransferKeyHash
{
    std::size_t operator()(const TransferKey& k) const noexcept
    {
        // Very simple hash: from_tag xor first 8 bytes of xfer_id.
        std::uint64_t v = k.from_tag;
        std::uint64_t x = 0;
        for (int i = 0; i < 8; ++i)
            x |= (static_cast<std::uint64_t>(k.xfer_id[static_cast<std::size_t>(i)]) << (8 * i));
        return static_cast<std::size_t>(v ^ x);
    }
};

class ChunkReassembler
{
public:
    explicit ChunkReassembler(ChunkCaps caps = {}) : m_caps(caps) {}

    // Adds a chunk. Returns true if accepted. If the transfer completes, out_full is set.
    bool AddChunk(std::uint64_t from_tag, const std::array<std::uint8_t, 16>& xfer_id, std::uint32_t chunk_index,
                  std::uint32_t chunk_count, std::uint32_t full_len, const std::vector<std::uint8_t>& chunk_bytes,
                  std::optional<std::vector<std::uint8_t>>& out_full, std::string* err = nullptr);

    void Clear() { m_transfers.clear(); }

private:
    struct Transfer
    {
        std::uint32_t chunk_count = 0;
        std::uint32_t full_len = 0;
        std::size_t total_bytes = 0;
        std::vector<std::optional<std::vector<std::uint8_t>>> chunks;
        std::size_t received = 0;
    };

    ChunkCaps m_caps;
    std::unordered_map<TransferKey, Transfer, TransferKeyHash> m_transfers;
};
} // namespace phos::p2p


