#include "test_harness.h"

#include "net/p2p/tile_codec_v1.h"

PHOS_TEST(tile_codec_roundtrip_v1_16)
{
    using namespace phos::p2p;
    const std::size_t tile_size = 16;
    const std::size_t n = tile_size * tile_size;

    std::vector<TileCellV1> cells;
    cells.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        cells[i].g = static_cast<std::uint32_t>(0x41 + (i % 50));
        cells[i].fg = static_cast<std::uint16_t>(i % 65535);
        cells[i].bg = static_cast<std::uint16_t>((i * 7) % 65535);
        cells[i].a = static_cast<std::uint16_t>(i % 0xFFFF);
    }

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeTileV1(tile_size, cells, bytes, &err));
    PHOS_REQUIRE_MSG(bytes.size() == TileByteLenV1(tile_size), err);

    std::vector<TileCellV1> decoded;
    PHOS_REQUIRE(DecodeTileV1(tile_size, bytes, decoded, &err));
    PHOS_REQUIRE_MSG(decoded.size() == cells.size(), err);

    for (std::size_t i = 0; i < n; ++i)
    {
        PHOS_REQUIRE(decoded[i].g == cells[i].g);
        PHOS_REQUIRE(decoded[i].fg == cells[i].fg);
        PHOS_REQUIRE(decoded[i].bg == cells[i].bg);
        PHOS_REQUIRE(decoded[i].a == cells[i].a);
    }
}


