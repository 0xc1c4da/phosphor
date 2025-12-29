#include "test_harness.h"

#include "net/p2p/canvas_tile_schema_v1.h"
#include "net/p2p/automerge_c_compat.h"
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

PHOS_TEST(crdt_tile_schema_missing_tile_returns_nullopt)
{
    using namespace phos::p2p;

    RoomSession a(RoomSessionConfig{.topic = "phos.room.tiles.neg", .local_peer_id = "peerA", .display_name = "alice", .privkey32 = Rand32()});
    std::string err;
    PHOS_REQUIRE(a.Init(&err));

    PHOS_REQUIRE(EnsureCanvasTileSchemaV1(a.Doc(), CanvasTileSchemaV1Config{.columns = 32, .rows = 32}, &err));
    PHOS_REQUIRE(a.Doc().Commit("init schema", &err));

    auto got = GetTileV1(a.Doc(), 0, 9, 9, &err);
    PHOS_REQUIRE(!got.has_value());
}

PHOS_TEST(crdt_tile_schema_non_bytes_tile_returns_nullopt)
{
    using namespace phos::p2p;

    RoomSession a(RoomSessionConfig{.topic = "phos.room.tiles.neg2", .local_peer_id = "peerA", .display_name = "alice", .privkey32 = Rand32()});
    std::string err;
    PHOS_REQUIRE(a.Init(&err));

    PHOS_REQUIRE(EnsureCanvasTileSchemaV1(a.Doc(), CanvasTileSchemaV1Config{.columns = 32, .rows = 32}, &err));

    // Directly write a non-bytes value at tiles["0,0"].
    auto ByteSpanSV = [](std::string_view s) -> AMbyteSpan {
        return AMbyteSpan{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
    };

    AMdoc* d = a.Doc().Raw();
    AMstack* stack = nullptr;

    auto get_obj_from_map = [&](const AMobjId* parent, std::string_view key) -> const AMobjId* {
        AMresult* get_r = AMmapGet(d, parent, ByteSpanSV(key), nullptr);
        AMitem* get_it = AMstackItem(&stack, get_r, nullptr, nullptr);
        if (!get_r || AMresultStatus(get_r) != AM_STATUS_OK || !get_it)
            return nullptr;
        if (AMitemValType(get_it) == AM_VAL_TYPE_VOID)
            return nullptr;
        return AMitemObjId(get_it);
    };

    const AMobjId* canvas = get_obj_from_map(AM_ROOT, "canvas");
    PHOS_REQUIRE(canvas != nullptr);
    const AMobjId* layers = get_obj_from_map(canvas, "layers");
    PHOS_REQUIRE(layers != nullptr);

    AMresult* layer_r = AMlistGet(d, layers, 0, nullptr);
    AMitem* layer_it = AMstackItem(&stack, layer_r, nullptr, nullptr);
    PHOS_REQUIRE(layer_r && AMresultStatus(layer_r) == AM_STATUS_OK && layer_it);
    const AMobjId* layer0 = AMitemObjId(layer_it);
    PHOS_REQUIRE(layer0 != nullptr);

    const AMobjId* tiles = get_obj_from_map(layer0, "tiles");
    PHOS_REQUIRE(tiles != nullptr);

    AMresult* put_r = AMmapPutInt(d, tiles, ByteSpanSV("0,0"), 123);
    AMstackItem(&stack, put_r, nullptr, nullptr);
    PHOS_REQUIRE(put_r && AMresultStatus(put_r) == AM_STATUS_OK);
    AMstackFree(&stack);

    PHOS_REQUIRE(a.Doc().Commit("put non-bytes tile", &err));

    err.clear();
    auto got = GetTileV1(a.Doc(), 0, 0, 0, &err);
    PHOS_REQUIRE(!got.has_value());
    PHOS_REQUIRE(!err.empty());
}


