#include "test_harness.h"

#include "net/p2p/canvas_tile_schema_v1.h"
#include "net/p2p/room_session.h"
#include "net/p2p/sim_transport.h"
#include "net/p2p/tile_codec_v1.h"

#include <random>

namespace
{
std::array<std::uint8_t, 32> Rand32()
{
    std::array<std::uint8_t, 32> out{};
    std::random_device rd;
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rd());
    return out;
}
} // namespace

PHOS_TEST(crdt_tile_schema_two_peers_roundtrip_bytes)
{
    using namespace phos::p2p;

    const std::string topic = "phos.room.tiles";
    SimTransport t;
    std::uint64_t now_ms = 0;

    RoomSession a(RoomSessionConfig{.topic = topic, .local_peer_id = "peerA", .display_name = "alice", .privkey32 = Rand32()});
    RoomSession b(RoomSessionConfig{.topic = topic, .local_peer_id = "peerB", .display_name = "bob", .privkey32 = Rand32()});

    std::string err;
    PHOS_REQUIRE(a.Init(&err));
    PHOS_REQUIRE(b.Init(&err));

    t.RegisterPeer("peerA", [&](const SimMessage& m) {
        if (m.topic == topic)
            a.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
    });
    t.RegisterPeer("peerB", [&](const SimMessage& m) {
        if (m.topic == topic)
            b.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
    });
    t.Subscribe("peerA", topic);
    t.Subscribe("peerB", topic);

    // Initialize schema and write a tile in A.
    PHOS_REQUIRE(EnsureCanvasTileSchemaV1(a.Doc(), CanvasTileSchemaV1Config{.columns = 32, .rows = 32}, &err));

    const std::size_t tile_size = 16;
    std::vector<TileCellV1> cells(tile_size * tile_size);
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        cells[i].g = static_cast<std::uint32_t>(i + 1);
        cells[i].fg = static_cast<std::uint16_t>(i & 0xFFFFu);
        cells[i].bg = static_cast<std::uint16_t>((i * 3) & 0xFFFFu);
        cells[i].a = static_cast<std::uint16_t>(i & 0x00FFu);
    }

    std::vector<std::uint8_t> tile_bytes;
    PHOS_REQUIRE(EncodeTileV1(tile_size, cells, tile_bytes, &err));
    PHOS_REQUIRE(PutTileV1(a.Doc(), 0, 0, 0, tile_bytes, &err));
    PHOS_REQUIRE(a.Doc().Commit("put tile 0,0", &err));

    // Let HELLO+SYNC converge.
    for (int step = 0; step < 1200; ++step)
    {
        a.Tick(now_ms);
        b.Tick(now_ms);

        for (auto& msg : a.TakeOutgoing())
            t.Broadcast("peerA", topic, msg);
        for (auto& msg : b.TakeOutgoing())
            t.Broadcast("peerB", topic, msg);

        now_ms += 10;
    }

    // Read back tile bytes on B and verify exact bytes + decode.
    auto got = GetTileV1(b.Doc(), 0, 0, 0, &err);
    PHOS_REQUIRE(got.has_value());
    PHOS_REQUIRE(*got == tile_bytes);

    std::vector<TileCellV1> cells2;
    PHOS_REQUIRE(DecodeTileV1(tile_size, *got, cells2, &err));
    PHOS_REQUIRE(cells2.size() == cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        const auto& a = cells[i];
        const auto& b = cells2[i];
        PHOS_REQUIRE(a.g == b.g);
        PHOS_REQUIRE(a.fg == b.fg);
        PHOS_REQUIRE(a.bg == b.bg);
        PHOS_REQUIRE(a.a == b.a);
    }
}


