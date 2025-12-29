#include "chunk_reassembly.h"

namespace phos::p2p
{
bool ChunkReassembler::AddChunk(std::uint64_t from_tag, const std::array<std::uint8_t, 16>& xfer_id,
                                std::uint32_t chunk_index, std::uint32_t chunk_count, std::uint32_t full_len,
                                const std::vector<std::uint8_t>& chunk_bytes,
                                std::optional<std::vector<std::uint8_t>>& out_full, std::string* err)
{
    out_full.reset();
    if (chunk_count == 0 || chunk_index >= chunk_count)
    {
        if (err)
            *err = "ChunkReassembler: invalid chunk index/count";
        return false;
    }
    if (chunk_count > m_caps.max_chunks_per_transfer)
    {
        if (err)
            *err = "ChunkReassembler: chunk_count exceeds cap";
        return false;
    }
    if (static_cast<std::size_t>(full_len) > m_caps.max_total_bytes_per_transfer)
    {
        if (err)
            *err = "ChunkReassembler: full_len exceeds cap";
        return false;
    }

    TransferKey key{from_tag, xfer_id};
    auto it = m_transfers.find(key);
    if (it == m_transfers.end())
    {
        if (m_transfers.size() >= m_caps.max_inflight_transfers)
        {
            if (err)
                *err = "ChunkReassembler: too many inflight transfers";
            return false;
        }
        Transfer t;
        t.chunk_count = chunk_count;
        t.full_len = full_len;
        t.chunks.resize(chunk_count);
        it = m_transfers.emplace(key, std::move(t)).first;
    }

    Transfer& t = it->second;
    if (t.chunk_count != chunk_count || t.full_len != full_len)
    {
        if (err)
            *err = "ChunkReassembler: transfer metadata mismatch";
        return false;
    }
    if (t.chunks.size() != chunk_count)
    {
        if (err)
            *err = "ChunkReassembler: internal chunk vector mismatch";
        return false;
    }

    // Drop duplicates.
    if (t.chunks[chunk_index].has_value())
        return true;

    t.total_bytes += chunk_bytes.size();
    if (t.total_bytes > m_caps.max_total_bytes_per_transfer)
    {
        if (err)
            *err = "ChunkReassembler: received bytes exceed cap";
        m_transfers.erase(it);
        return false;
    }

    t.chunks[chunk_index] = chunk_bytes;
    t.received += 1;

    if (t.received != t.chunk_count)
        return true;

    // Assemble in order.
    std::vector<std::uint8_t> full;
    full.reserve(t.total_bytes);
    for (std::uint32_t i = 0; i < t.chunk_count; ++i)
    {
        if (!t.chunks[i].has_value())
        {
            if (err)
                *err = "ChunkReassembler: missing chunk at completion";
            m_transfers.erase(it);
            return false;
        }
        const auto& part = *t.chunks[i];
        full.insert(full.end(), part.begin(), part.end());
    }

    // Optional sanity check against declared full_len.
    if (t.full_len != 0 && full.size() != static_cast<std::size_t>(t.full_len))
    {
        // The spec recommends full_len to represent unchunked payload size; we enforce equality here for safety.
        if (err)
            *err = "ChunkReassembler: assembled length mismatch";
        m_transfers.erase(it);
        return false;
    }

    out_full = std::move(full);
    m_transfers.erase(it);
    return true;
}
} // namespace phos::p2p


