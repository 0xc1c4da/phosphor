#include "test_harness.h"

#include "net/p2p/faulty_sim_transport.h"
#include "net/p2p/room_session.h"

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

PHOS_TEST(room_session_faulty_reorder_dup_converge)
{
    using namespace phos::p2p;

    const std::string topic = "phos.room.faulty";
    std::uint64_t now_ms = 0;

    FaultySimTransport t(FaultySimConfig{
        .seed = 123,
        .drop_prob = 0.0,
        .dup_prob = 0.10,
        .max_delay_ms = 30,
    });

    RoomSession a(RoomSessionConfig{
        .topic = topic,
        .local_peer_id = "peerA",
        .display_name = "alice",
        .privkey32 = Rand32(),
        .hello_interval_ms = 200,
        .sync_interval_ms = 50,
    });
    RoomSession b(RoomSessionConfig{
        .topic = topic,
        .local_peer_id = "peerB",
        .display_name = "bob",
        .privkey32 = Rand32(),
        .hello_interval_ms = 200,
        .sync_interval_ms = 50,
    });
    RoomSession c(RoomSessionConfig{
        .topic = topic,
        .local_peer_id = "peerC",
        .display_name = "cathy",
        .privkey32 = Rand32(),
        .hello_interval_ms = 200,
        .sync_interval_ms = 50,
    });

    std::string err;
    PHOS_REQUIRE(a.Init(&err));
    PHOS_REQUIRE(b.Init(&err));
    PHOS_REQUIRE(c.Init(&err));

    t.RegisterPeer("peerA", [&](const SimMessage& m) {
        if (m.topic == topic)
            a.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
    });
    t.RegisterPeer("peerB", [&](const SimMessage& m) {
        if (m.topic == topic)
            b.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
    });
    t.RegisterPeer("peerC", [&](const SimMessage& m) {
        if (m.topic == topic)
            c.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
    });
    t.Subscribe("peerA", topic);
    t.Subscribe("peerB", topic);
    t.Subscribe("peerC", topic);

    // Make a commit on A, then let periodic HELLO+SYNC converge the others.
    PHOS_REQUIRE(a.Doc().RootPutInt("x", 123, &err));
    PHOS_REQUIRE(a.Doc().Commit("set x=123", &err));

    for (int step = 0; step < 2500; ++step)
    {
        a.Tick(now_ms);
        b.Tick(now_ms);
        c.Tick(now_ms);

        for (auto& msg : a.TakeOutgoing())
            t.Broadcast("peerA", topic, msg, now_ms);
        for (auto& msg : b.TakeOutgoing())
            t.Broadcast("peerB", topic, msg, now_ms);
        for (auto& msg : c.TakeOutgoing())
            t.Broadcast("peerC", topic, msg, now_ms);

        t.Tick(now_ms);
        now_ms += 10;
    }

    PHOS_REQUIRE(a.Doc().Equal(b.Doc()));
    PHOS_REQUIRE(a.Doc().Equal(c.Doc()));
}

PHOS_TEST(room_session_faulty_drop_converge)
{
    using namespace phos::p2p;

    const std::string topic = "phos.room.faulty.drop";
    std::uint64_t now_ms = 0;

    FaultySimTransport t(FaultySimConfig{
        .seed = 456,
        .drop_prob = 0.05,
        .dup_prob = 0.05,
        .max_delay_ms = 50,
    });

    RoomSession a(RoomSessionConfig{
        .topic = topic,
        .local_peer_id = "peerA",
        .display_name = "alice",
        .privkey32 = Rand32(),
        .hello_interval_ms = 200,
        .sync_interval_ms = 50,
    });
    RoomSession b(RoomSessionConfig{
        .topic = topic,
        .local_peer_id = "peerB",
        .display_name = "bob",
        .privkey32 = Rand32(),
        .hello_interval_ms = 200,
        .sync_interval_ms = 50,
    });

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

    PHOS_REQUIRE(a.Doc().RootPutInt("x", 999, &err));
    PHOS_REQUIRE(a.Doc().Commit("set x=999", &err));

    for (int step = 0; step < 3500; ++step)
    {
        a.Tick(now_ms);
        b.Tick(now_ms);

        for (auto& msg : a.TakeOutgoing())
            t.Broadcast("peerA", topic, msg, now_ms);
        for (auto& msg : b.TakeOutgoing())
            t.Broadcast("peerB", topic, msg, now_ms);

        t.Tick(now_ms);
        now_ms += 10;
    }

    PHOS_REQUIRE(a.Doc().Equal(b.Doc()));
}


