#include "test_harness.h"

#include "net/p2p/chunk_reassembly.h"

#include <array>

PHOS_TEST(chunk_reassembly_out_of_order)
{
    using namespace phos::p2p;
    ChunkReassembler r;

    std::vector<std::uint8_t> full(1024);
    for (std::size_t i = 0; i < full.size(); ++i)
        full[i] = static_cast<std::uint8_t>(i & 0xFF);

    std::array<std::uint8_t, 16> xfer{};
    xfer[0] = 7;
    const std::uint32_t chunk_count = 4;
    const std::uint32_t full_len = static_cast<std::uint32_t>(full.size());

    // Split into 4 chunks.
    std::vector<std::vector<std::uint8_t>> chunks;
    for (std::uint32_t i = 0; i < chunk_count; ++i)
    {
        const std::size_t start = (full.size() * i) / chunk_count;
        const std::size_t end = (full.size() * (i + 1)) / chunk_count;
        chunks.emplace_back(full.begin() + start, full.begin() + end);
    }

    std::optional<std::vector<std::uint8_t>> out;
    std::string err;
    // Add out of order: 2,0,3,1
    PHOS_REQUIRE(r.AddChunk(1, xfer, 2, chunk_count, full_len, chunks[2], out, &err));
    PHOS_REQUIRE(!out.has_value());
    PHOS_REQUIRE(r.AddChunk(1, xfer, 0, chunk_count, full_len, chunks[0], out, &err));
    PHOS_REQUIRE(!out.has_value());
    PHOS_REQUIRE(r.AddChunk(1, xfer, 3, chunk_count, full_len, chunks[3], out, &err));
    PHOS_REQUIRE(!out.has_value());
    PHOS_REQUIRE(r.AddChunk(1, xfer, 1, chunk_count, full_len, chunks[1], out, &err));
    PHOS_REQUIRE_MSG(out.has_value(), err);
    PHOS_REQUIRE(out->size() == full.size());
    PHOS_REQUIRE(*out == full);
}


