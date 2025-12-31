// Headless multiplayer lab for Phosphor P2P CRDT sync.
//
// Goals:
// - Exercise core data structures (wire framing, signatures, chunking, automerge sync)
// - Run without GUI
// - Support a deterministic sim mode and a real simplep2p mode
//
// Examples:
//   nix develop -c make mp-lab -j
//   ./phosphor_mp_lab --mode sim
//   ./phosphor_mp_lab --mode p2p --topic phos.room.debug --name alice --host
//   ./phosphor_mp_lab --mode p2p --topic phos.room.debug --name bob

#include "net/p2p/automerge_adapter.h"
#include "net/p2p/blake3_util.h"
#include "net/p2p/crypto_secp256k1.h"
#include "net/p2p/room_session.h"
#include "net/p2p/sim_transport.h"
#include "net/p2p/wire_frame_v1.h"

// Work around missing standard includes in the nix-provided `simplep2p.hpp`.
#include <algorithm>
#include <map>
#include <simplep2p.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// `simplep2p.hpp` declares this static member but (in the version packaged in
// nix) does not provide a definition. Provide it here so the mp-lab binary links.
namespace p2p
{
std::map<P2PNetwork, Network*> Network::networks;
} // namespace p2p

namespace
{
using namespace phos::p2p;

std::array<std::uint8_t, 16> Rand16()
{
    std::array<std::uint8_t, 16> out{};
    std::random_device rd;
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rd());
    return out;
}

std::array<std::uint8_t, 32> Rand32()
{
    std::array<std::uint8_t, 32> out{};
    std::random_device rd;
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rd());
    return out;
}

std::uint64_t TagFromPeerId(std::string_view domain, std::string_view peer_id)
{
    std::string s;
    s.reserve(domain.size() + peer_id.size());
    s.append(domain);
    s.append(peer_id);
    return TagU64FromFirst8LE(Blake3_32(s));
}

std::array<std::uint8_t, 20> RoomTagFromTopic(std::string_view topic)
{
    std::string s;
    s.reserve(12 + topic.size());
    s.append("phos/room_tag");
    s.append(topic);
    auto h = Blake3_32(s);
    std::array<std::uint8_t, 20> out{};
    std::memcpy(out.data(), h.data(), out.size());
    return out;
}

std::uint64_t ToTagFromPeerId(std::string_view peer_id) { return TagFromPeerId("phos/to_tag", peer_id); }
std::uint64_t FromTagFromPeerId(std::string_view peer_id) { return TagFromPeerId("phos/from_tag", peer_id); }

std::uint64_t UserTagFromPubkey33(std::span<const std::uint8_t> pubkey33)
{
    std::string prefix = "phos/user_tag";
    std::vector<std::uint8_t> buf;
    buf.reserve(prefix.size() + pubkey33.size());
    buf.insert(buf.end(), prefix.begin(), prefix.end());
    buf.insert(buf.end(), pubkey33.begin(), pubkey33.end());
    return TagU64FromFirst8LE(Blake3_32(buf));
}

std::uint64_t NowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Minimal Hello payload (matches spec layout).
std::vector<std::uint8_t> EncodeHelloPayload(std::span<const std::uint8_t> pubkey33, std::string_view name)
{
    std::vector<std::uint8_t> out;
    out.reserve(4 + 1 + 2 + 2 + 4 + 33 + 2 + name.size() + 2);
    out.insert(out.end(), {'H', 'E', 'L', 'O'});
    out.push_back(1); // hello_ver
    // doc_schema u16
    out.push_back(1);
    out.push_back(0);
    // app_ver u16
    out.push_back(1);
    out.push_back(0);
    // caps u32
    out.insert(out.end(), {0, 0, 0, 0});
    out.insert(out.end(), pubkey33.begin(), pubkey33.end());
    // name_len u16
    const std::uint16_t nl = static_cast<std::uint16_t>(name.size());
    out.push_back(static_cast<std::uint8_t>(nl & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((nl >> 8) & 0xFFu));
    out.insert(out.end(), name.begin(), name.end());
    // extra_len u16 = 0
    out.push_back(0);
    out.push_back(0);
    return out;
}

struct ParsedHello
{
    std::array<std::uint8_t, 33> pubkey33{};
    std::string name;
};

bool DecodeHelloPayload(std::span<const std::uint8_t> in, ParsedHello& out, std::string* err)
{
    if (in.size() < 4 + 1 + 2 + 2 + 4 + 33 + 2 + 2)
    {
        if (err)
            *err = "hello too short";
        return false;
    }
    std::size_t off = 0;
    if (!(in[0] == 'H' && in[1] == 'E' && in[2] == 'L' && in[3] == 'O'))
    {
        if (err)
            *err = "hello bad magic";
        return false;
    }
    off += 4;
    const std::uint8_t ver = in[off++];
    if (ver != 1)
    {
        if (err)
            *err = "hello unsupported ver";
        return false;
    }
    off += 2; // doc_schema
    off += 2; // app_ver
    off += 4; // caps
    std::memcpy(out.pubkey33.data(), in.data() + off, 33);
    off += 33;
    const std::uint16_t name_len = static_cast<std::uint16_t>(in[off]) | (static_cast<std::uint16_t>(in[off + 1]) << 8);
    off += 2;
    if (off + name_len + 2 > in.size())
    {
        if (err)
            *err = "hello name_len out of range";
        return false;
    }
    out.name.assign(reinterpret_cast<const char*>(in.data() + off), name_len);
    off += name_len;
    const std::uint16_t extra_len = static_cast<std::uint16_t>(in[off]) | (static_cast<std::uint16_t>(in[off + 1]) << 8);
    off += 2;
    if (off + extra_len != in.size())
    {
        // tolerate trailing bytes, but keep strict for lab
        if (err)
            *err = "hello extra_len mismatch";
        return false;
    }
    return true;
}

struct PeerInfo
{
    std::string peer_id;
    std::array<std::uint8_t, 33> pubkey33{};
    std::string name;
    std::uint64_t user_tag = 0;
    std::uint64_t last_seen_ms = 0;
};

struct Node
{
    std::string topic;
    std::array<std::uint8_t, 20> room_tag{};

    std::string peer_id;
    std::uint64_t from_tag = 0;
    std::array<std::uint8_t, 32> privkey32{};
    std::array<std::uint8_t, 33> pubkey33{};
    std::uint64_t user_tag = 0;

    std::string name;

    AutomergeAdapter am;
    std::unordered_map<std::string, PeerInfo> peers; // peer_id -> info

    bool Init(std::string_view in_topic, std::string in_peer_id, std::string in_name, std::string* err)
    {
        topic = std::string(in_topic);
        room_tag = RoomTagFromTopic(topic);
        peer_id = std::move(in_peer_id);
        name = std::move(in_name);

        privkey32 = Rand32();
        if (!SecpPubkeyFromPriv(privkey32, pubkey33, err))
            return false;
        user_tag = UserTagFromPubkey33(pubkey33);
        from_tag = FromTagFromPeerId(peer_id);

        // actor id bytes derived from signing pubkey, matching spec.
        std::vector<std::uint8_t> actor_bytes;
        const auto h = Blake3_32(std::span<const std::uint8_t>(pubkey33.data(), pubkey33.size()));
        actor_bytes.assign(h.begin(), h.end());
        if (!am.CreateWithActorId(actor_bytes, err))
            return false;
        return true;
    }

    std::vector<std::uint8_t> BuildSignedFrame(MsgType type, std::uint64_t to_tag, std::span<const std::uint8_t> payload,
                                               const std::optional<ChunkMetaV1>& chunk = std::nullopt)
    {
        FrameHeaderV1 h;
        h.type = type;
        h.flags = PHOS_F_SIGNED | (chunk ? PHOS_F_CHUNKED : 0);
        h.enc = 0;
        h.comp = 0;
        h.room_tag = room_tag;
        h.t_ms = NowMs();
        h.to_tag = to_tag;
        h.from_tag = from_tag;
        h.user_tag = user_tag;
        h.msg_id = Rand16();

        std::vector<std::uint8_t> signed_bytes;
        std::string tmp_err;
        BuildSignedBytesV1(h, std::nullopt, chunk, {}, payload, signed_bytes, &tmp_err);

        const auto digest = Blake3_32(std::span<const std::uint8_t>(signed_bytes.data(), signed_bytes.size()));
        std::array<std::uint8_t, 64> sig{};
        SecpSignCompact(privkey32, digest, sig, &tmp_err);

        std::vector<std::uint8_t> out;
        EncodeFrameV1(h, std::nullopt, chunk, {}, payload, sig, out, &tmp_err);
        return out;
    }

    std::vector<std::uint8_t> BuildHelloFrame()
    {
        auto payload = EncodeHelloPayload(pubkey33, name);
        return BuildSignedFrame(MsgType::Hello, 0 /*broadcast*/, payload);
    }

    std::vector<std::uint8_t> BuildSyncFrame(std::string_view to_peer_id, std::span<const std::uint8_t> sync_bytes)
    {
        return BuildSignedFrame(MsgType::Sync, ToTagFromPeerId(to_peer_id), sync_bytes);
    }

    bool VerifyFrame(const ParsedFrameV1& f, std::string_view from_peer_id, std::string* err)
    {
        // HELLO includes pubkey; for other messages we require we've seen HELLO for that peer already.
        std::array<std::uint8_t, 33> pub{};
        if (f.hdr.type == MsgType::Hello)
        {
            ParsedHello hello{};
            std::string tmp;
            if (!DecodeHelloPayload(f.payload, hello, &tmp))
            {
                if (err)
                    *err = "VerifyFrame: bad hello payload: " + tmp;
                return false;
            }
            pub = hello.pubkey33;
        }
        else
        {
            auto it = peers.find(std::string(from_peer_id));
            if (it == peers.end())
            {
                if (err)
                    *err = "VerifyFrame: missing peer hello";
                return false;
            }
            pub = it->second.pubkey33;
        }

        std::vector<std::uint8_t> signed_bytes;
        if (!BuildSignedBytesV1(f.hdr, f.key_id, f.chunk, f.nonce, f.payload, signed_bytes, err))
            return false;

        const auto digest = Blake3_32(std::span<const std::uint8_t>(signed_bytes.data(), signed_bytes.size()));
        if (!SecpVerifyCompact(pub, digest, f.sig, err))
            return false;
        return true;
    }

    void OnFrame(const std::string& from_peer_id, const ParsedFrameV1& f)
    {
        // Only accept correct room_tag.
        if (f.hdr.room_tag != room_tag)
            return;
        if (from_peer_id == peer_id)
            return;

        if (f.hdr.type == MsgType::Hello)
        {
            ParsedHello hello{};
            std::string err;
            if (!DecodeHelloPayload(f.payload, hello, &err))
                return;
            const std::uint64_t expected_user_tag =
                UserTagFromPubkey33(std::span<const std::uint8_t>(hello.pubkey33.data(), hello.pubkey33.size()));
            if (expected_user_tag != f.hdr.user_tag)
                return;

            PeerInfo pi;
            pi.peer_id = from_peer_id;
            pi.pubkey33 = hello.pubkey33;
            pi.name = hello.name;
            pi.user_tag = expected_user_tag;
            pi.last_seen_ms = NowMs();
            peers[from_peer_id] = pi;
            return;
        }

        if (f.hdr.type == MsgType::Sync)
        {
            std::string err;
            am.ReceiveSync(from_peer_id, f.payload, &err);
            return;
        }
    }

    void MutateOnce()
    {
        std::string err;
        am.RootPutInt("x", static_cast<std::int64_t>(NowMs()), &err);
        am.Commit("tick", &err);
    }
};

void PrintUsage()
{
    std::cout << "phosphor_mp_lab usage:\n"
                 "  --mode sim|p2p         (default: sim)\n"
                 "  --topic <topic>        (default: phos.room.debug)\n"
                 "  --name <name>          (default: anon)\n"
                 "  --host                 (in p2p mode: periodically mutates doc)\n"
                 "  --seconds <n>          (sim mode duration, default 1)\n"
                 "\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string mode = "sim";
    std::string topic = "phos.room.debug";
    std::string name = "anon";
    bool host = false;
    int seconds = 1;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc)
            mode = argv[++i];
        else if (a == "--topic" && i + 1 < argc)
            topic = argv[++i];
        else if (a == "--name" && i + 1 < argc)
            name = argv[++i];
        else if (a == "--seconds" && i + 1 < argc)
            seconds = std::stoi(argv[++i]);
        else if (a == "--host")
            host = true;
        else if (a == "--help" || a == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::cout << "Unknown arg: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (mode == "sim")
    {
        SimTransport t;

        RoomSession a(RoomSessionConfig{
            .topic = topic,
            .local_peer_id = "peerA",
            .display_name = "alice",
            .privkey32 = Rand32(),
        });
        RoomSession b(RoomSessionConfig{
            .topic = topic,
            .local_peer_id = "peerB",
            .display_name = "bob",
            .privkey32 = Rand32(),
        });

        std::string err;
        if (!a.Init(&err) || !b.Init(&err))
        {
            std::cout << "init failed: " << err << "\n";
            return 1;
        }

        std::uint64_t now_ms = 0;

        t.RegisterPeer("peerA", [&](const SimMessage& m) {
            if (m.topic != topic)
                return;
            a.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
        });
        t.RegisterPeer("peerB", [&](const SimMessage& m) {
            if (m.topic != topic)
                return;
            b.OnIncomingBytes(m.from_peer_id, m.bytes, now_ms);
        });
        t.Subscribe("peerA", topic);
        t.Subscribe("peerB", topic);

        // Mutate A once then sync.
        a.Doc().RootPutInt("x", 1, &err);
        a.Doc().Commit("set x=1", &err);

        const std::uint64_t end_ms = static_cast<std::uint64_t>(seconds) * 1000;
        while (now_ms <= end_ms)
        {
            a.Tick(now_ms);
            b.Tick(now_ms);

            for (auto& msg : a.TakeOutgoing())
                t.Broadcast("peerA", topic, msg);
            for (auto& msg : b.TakeOutgoing())
                t.Broadcast("peerB", topic, msg);

            now_ms += 10;
        }

        const bool eq = a.Doc().Equal(b.Doc());
        std::cout << "[sim] converged=" << (eq ? "yes" : "no") << " peersA=" << a.Peers().size()
                  << " peersB=" << b.Peers().size() << "\n";
        return eq ? 0 : 1;
    }

    if (mode == "p2p")
    {
        static std::atomic<bool> running{true};
        std::signal(SIGINT, [](int) { running = false; });

        p2p::Network net(p2p::default_listen_address, p2p::default_discovery_topic, p2p::Key{}, nullptr,
                         std::chrono::seconds(30), false, false);
        const auto my_peer_id = std::string(net.local_id());
        RoomSession session(RoomSessionConfig{
            .topic = topic,
            .local_peer_id = my_peer_id,
            .display_name = name,
            .privkey32 = Rand32(),
        });
        std::string err;
        if (!session.Init(&err))
        {
            std::cout << "init failed: " << err << "\n";
            return 1;
        }

        const auto room = net.subscribe_to_topic(topic);
        std::cout << "[p2p] my_peer_id=" << session.LocalPeerId() << " topic=" << room.name()
                  << " user_tag=" << session.MyUserTag() << "\n";

        // simplep2p callbacks can fire on worker threads. Copy bytes immediately and enqueue.
        std::mutex inbox_mu;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> inbox;

        net.on_message.connect([&](p2p::Network&, p2p::Message& msg) {
            // copy immediately (simplep2p frees memory after callback returns)
            const std::string from = std::string(msg.sender());
            const auto data = msg.data();
            std::vector<std::uint8_t> bytes;
            bytes.reserve(data.size());
            for (auto b : data)
                bytes.push_back(static_cast<std::uint8_t>(b));
            {
                std::lock_guard<std::mutex> lk(inbox_mu);
                inbox.emplace_back(from, std::move(bytes));
            }
        });

        auto last_tick = std::chrono::steady_clock::now();

        while (running)
        {
            const auto now = std::chrono::steady_clock::now();
            {
                // Drain inbox and feed to session on this (main) thread.
                std::vector<std::pair<std::string, std::vector<std::uint8_t>>> local;
                {
                    std::lock_guard<std::mutex> lk(inbox_mu);
                    local.swap(inbox);
                }
                const std::uint64_t now_ms = NowMs();
                for (auto& [from, bytes] : local)
                    session.OnIncomingBytes(from, bytes, now_ms);
            }

            if (host && now - last_tick > std::chrono::seconds(1))
            {
                // Mutate doc occasionally to ensure we exercise sync on real transport.
                session.Doc().RootPutInt("x", static_cast<std::int64_t>(NowMs()), &err);
                session.Doc().Commit("tick", &err);
                last_tick = now;
            }

            // Drive periodic HELLO + SYNC (RoomSession decides what to send).
            const std::uint64_t now_ms = NowMs();
            session.Tick(now_ms);
            for (auto& frame : session.TakeOutgoing())
                net.broadcast_message(std::span<std::byte>((std::byte*)frame.data(), frame.size()), room);

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        net.shutdown();
        std::cout << "[p2p] shutdown.\n";
        return 0;
    }

    std::cout << "Unknown mode: " << mode << "\n";
    PrintUsage();
    return 2;
}


