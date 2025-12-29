#include "room_session.h"

#include "blake3_util.h"
#include "crypto_secp256k1.h"
#include "wire_frame_v1.h"

#include <cstring>
#include <optional>
#include <random>

namespace phos::p2p
{
namespace
{
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
    const std::string prefix = "phos/user_tag";
    std::vector<std::uint8_t> buf;
    buf.reserve(prefix.size() + pubkey33.size());
    buf.insert(buf.end(), prefix.begin(), prefix.end());
    buf.insert(buf.end(), pubkey33.begin(), pubkey33.end());
    return TagU64FromFirst8LE(Blake3_32(buf));
}

std::vector<std::uint8_t> ActorIdFromPubkey33(std::span<const std::uint8_t> pubkey33)
{
    const std::string prefix = "phosphor/actor";
    std::vector<std::uint8_t> buf;
    buf.reserve(prefix.size() + pubkey33.size());
    buf.insert(buf.end(), prefix.begin(), prefix.end());
    buf.insert(buf.end(), pubkey33.begin(), pubkey33.end());
    const auto h = Blake3_32(buf);
    return std::vector<std::uint8_t>(h.begin(), h.end());
}

std::array<std::uint8_t, 16> Rand16()
{
    std::array<std::uint8_t, 16> out{};
    std::random_device rd;
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rd());
    return out;
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
    if (name_len > 128)
    {
        if (err)
            *err = "hello name_len too large";
        return false;
    }
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
        if (err)
            *err = "hello extra_len mismatch";
        return false;
    }
    return true;
}

bool VerifyFrameSig(const ParsedFrameV1& f, std::span<const std::uint8_t> pubkey33, std::string* err)
{
    std::vector<std::uint8_t> signed_bytes;
    if (!BuildSignedBytesV1(f.hdr, f.key_id, f.chunk, f.nonce, f.payload, signed_bytes, err))
        return false;
    const auto digest = Blake3_32(std::span<const std::uint8_t>(signed_bytes.data(), signed_bytes.size()));
    return SecpVerifyCompact(pubkey33, digest, f.sig, err);
}

std::vector<std::uint8_t> BuildSignedFrame(const std::array<std::uint8_t, 20>& room_tag, std::uint64_t from_tag,
                                           std::uint64_t user_tag, std::span<const std::uint8_t> privkey32,
                                           MsgType type, std::uint64_t t_ms, std::uint64_t to_tag,
                                           std::span<const std::uint8_t> payload,
                                           const std::optional<ChunkMetaV1>& chunk = std::nullopt)
{
    FrameHeaderV1 h;
    h.type = type;
    h.flags = PHOS_F_SIGNED | (chunk ? PHOS_F_CHUNKED : 0);
    h.enc = 0;
    h.comp = 0;
    h.room_tag = room_tag;
    h.t_ms = t_ms;
    h.to_tag = to_tag;
    h.from_tag = from_tag;
    h.user_tag = user_tag;
    h.msg_id = Rand16();

    std::vector<std::uint8_t> signed_bytes;
    std::string tmp_err;
    (void)BuildSignedBytesV1(h, std::nullopt, chunk, {}, payload, signed_bytes, &tmp_err);

    const auto digest = Blake3_32(std::span<const std::uint8_t>(signed_bytes.data(), signed_bytes.size()));
    std::array<std::uint8_t, 64> sig{};
    (void)SecpSignCompact(privkey32, digest, sig, &tmp_err);

    std::vector<std::uint8_t> out;
    (void)EncodeFrameV1(h, std::nullopt, chunk, {}, payload, sig, out, &tmp_err);
    return out;
}

} // namespace

RoomSession::RoomSession(RoomSessionConfig cfg) : m_cfg(std::move(cfg)), m_reasm(m_cfg.chunk_caps) {}

bool RoomSession::Init(std::string* err)
{
    if (m_cfg.topic.empty() || m_cfg.local_peer_id.empty())
    {
        if (err)
            *err = "RoomSession::Init: topic and local_peer_id are required";
        return false;
    }

    m_room_tag = RoomTagFromTopic(m_cfg.topic);
    m_my_to_tag = ToTagFromPeerId(m_cfg.local_peer_id);
    m_my_from_tag = FromTagFromPeerId(m_cfg.local_peer_id);

    if (!SecpPubkeyFromPriv(m_cfg.privkey32, m_my_pubkey33, err))
        return false;
    m_my_user_tag = UserTagFromPubkey33(m_my_pubkey33);

    const auto actor_id_bytes = ActorIdFromPubkey33(m_my_pubkey33);
    if (!m_doc.CreateWithActorId(actor_id_bytes, err))
        return false;

    // Force initial hello/sync on first tick.
    m_last_hello_ms = 0;
    m_last_sync_ms = 0;
    return true;
}

void RoomSession::Enqueue(std::vector<std::uint8_t> frame_bytes) { m_out.push_back(std::move(frame_bytes)); }

void RoomSession::EnqueueHello(std::uint64_t now_ms)
{
    const auto payload = EncodeHelloPayload(m_my_pubkey33, m_cfg.display_name);
    Enqueue(BuildSignedFrame(m_room_tag, m_my_from_tag, m_my_user_tag, m_cfg.privkey32, MsgType::Hello, now_ms, 0, payload));
}

void RoomSession::EnqueueSyncToPeer(std::string_view peer_id, std::span<const std::uint8_t> sync_bytes, std::uint64_t now_ms)
{
    if (sync_bytes.empty())
        return;

    // Conservative chunk payload sizing to stay under max_wire_bytes (header + meta + sig overhead).
    constexpr std::size_t kOverhead = 140;
    const std::size_t max_payload = (m_cfg.max_wire_bytes > kOverhead) ? (m_cfg.max_wire_bytes - kOverhead) : 1024;

    if (sync_bytes.size() <= max_payload)
    {
        Enqueue(BuildSignedFrame(m_room_tag, m_my_from_tag, m_my_user_tag, m_cfg.privkey32, MsgType::Sync, now_ms,
                                ToTagFromPeerId(peer_id), sync_bytes));
        return;
    }

    const std::array<std::uint8_t, 16> xfer = Rand16();
    const std::uint32_t full_len = static_cast<std::uint32_t>(sync_bytes.size());
    const std::uint32_t chunk_count =
        static_cast<std::uint32_t>((sync_bytes.size() + max_payload - 1) / max_payload);

    for (std::uint32_t i = 0; i < chunk_count; ++i)
    {
        const std::size_t off = static_cast<std::size_t>(i) * max_payload;
        const std::size_t n = std::min(max_payload, sync_bytes.size() - off);
        const auto chunk_span = sync_bytes.subspan(off, n);

        ChunkMetaV1 cm;
        cm.xfer_id = xfer;
        cm.chunk_index = i;
        cm.chunk_count = chunk_count;
        cm.full_len = full_len;

        Enqueue(BuildSignedFrame(m_room_tag, m_my_from_tag, m_my_user_tag, m_cfg.privkey32, MsgType::SyncChunk, now_ms,
                                ToTagFromPeerId(peer_id), chunk_span, cm));
    }
}

void RoomSession::EnqueueSyncToAll(std::uint64_t now_ms)
{
    for (auto& [pid, peer] : m_peers)
    {
        if (!peer.handshake_ok)
            continue;
        std::string err;
        if (auto msg = m_doc.GenerateSync(pid, &err))
            EnqueueSyncToPeer(pid, *msg, now_ms);
    }
}

void RoomSession::OnHello(std::string_view from_peer_id, std::uint64_t now_ms, std::span<const std::uint8_t> payload,
                          std::uint64_t from_tag, std::uint64_t user_tag)
{
    ParsedHello hello{};
    std::string err;
    if (!DecodeHelloPayload(payload, hello, &err))
        return;

    const std::uint64_t expected_user_tag =
        UserTagFromPubkey33(std::span<const std::uint8_t>(hello.pubkey33.data(), hello.pubkey33.size()));
    if (expected_user_tag != user_tag)
        return;

    RoomPeer p;
    p.peer_id = std::string(from_peer_id);
    p.pubkey33 = hello.pubkey33;
    p.name = hello.name;
    p.from_tag = from_tag;
    p.user_tag = expected_user_tag;
    p.last_seen_ms = now_ms;
    p.handshake_ok = true;
    m_peers[p.peer_id] = std::move(p);

    // Try to immediately seed the sync handshake (late-join bootstrap).
    std::string sync_err;
    if (auto sm = m_doc.GenerateSync(from_peer_id, &sync_err))
        EnqueueSyncToPeer(from_peer_id, *sm, now_ms);
}

void RoomSession::OnSync(std::string_view from_peer_id, std::span<const std::uint8_t> sync_bytes, std::uint64_t now_ms)
{
    std::string err;
    (void)m_doc.ReceiveSync(from_peer_id, sync_bytes, &err);

    // Automerge sync is interactive; immediately attempt to reply.
    if (auto sm = m_doc.GenerateSync(from_peer_id, &err))
        EnqueueSyncToPeer(from_peer_id, *sm, now_ms);
}

void RoomSession::OnSyncChunk(std::string_view from_peer_id, std::uint64_t from_tag, std::span<const std::uint8_t> chunk_bytes,
                              const std::array<std::uint8_t, 16>& xfer_id, std::uint32_t chunk_index,
                              std::uint32_t chunk_count, std::uint32_t full_len, std::uint64_t now_ms)
{
    std::optional<std::vector<std::uint8_t>> full;
    std::string err;
    std::vector<std::uint8_t> chunk_vec(chunk_bytes.begin(), chunk_bytes.end());
    if (!m_reasm.AddChunk(from_tag, xfer_id, chunk_index, chunk_count, full_len, chunk_vec, full, &err))
        return;
    if (full)
        OnSync(from_peer_id, *full, now_ms);
}

void RoomSession::OnIncomingBytes(std::string_view from_peer_id, std::span<const std::uint8_t> bytes, std::uint64_t now_ms)
{
    if (from_peer_id == m_cfg.local_peer_id)
        return;

    ParsedFrameV1 f;
    std::string err;
    if (!DecodeFrameV1(bytes, f, &err))
        return;

    if (f.hdr.room_tag != m_room_tag)
        return;

    // Fast directed filtering.
    if (f.hdr.to_tag != 0 && f.hdr.to_tag != m_my_to_tag)
        return;

    // Verify signature.
    if (f.hdr.type == MsgType::Hello)
    {
        ParsedHello hello{};
        std::string e2;
        if (!DecodeHelloPayload(f.payload, hello, &e2))
            return;
        if (!VerifyFrameSig(f, std::span<const std::uint8_t>(hello.pubkey33.data(), hello.pubkey33.size()), &err))
            return;
        OnHello(from_peer_id, now_ms, f.payload, f.hdr.from_tag, f.hdr.user_tag);
        return;
    }

    auto it = m_peers.find(std::string(from_peer_id));
    if (it == m_peers.end() || !it->second.handshake_ok)
        return;
    it->second.last_seen_ms = now_ms;

    if (!VerifyFrameSig(f, std::span<const std::uint8_t>(it->second.pubkey33.data(), it->second.pubkey33.size()), &err))
        return;

    if (f.hdr.type == MsgType::Sync)
    {
        OnSync(from_peer_id, f.payload, now_ms);
    }
    else if (f.hdr.type == MsgType::SyncChunk && f.chunk)
    {
        OnSyncChunk(from_peer_id, f.hdr.from_tag, f.payload, f.chunk->xfer_id, f.chunk->chunk_index, f.chunk->chunk_count,
                    f.chunk->full_len, now_ms);
    }
}

void RoomSession::Tick(std::uint64_t now_ms)
{
    if (!m_doc.Valid())
        return;

    if (m_last_hello_ms == 0 || (now_ms - m_last_hello_ms) >= m_cfg.hello_interval_ms)
    {
        EnqueueHello(now_ms);
        m_last_hello_ms = now_ms;
    }

    if (m_last_sync_ms == 0 || (now_ms - m_last_sync_ms) >= m_cfg.sync_interval_ms)
    {
        EnqueueSyncToAll(now_ms);
        m_last_sync_ms = now_ms;
    }

    // Stale peer handling (simple: mark handshake_ok false if stale).
    for (auto& [pid, p] : m_peers)
    {
        const bool stale = (p.last_seen_ms != 0) && (now_ms - p.last_seen_ms > m_cfg.peer_stale_ms);
        if (stale)
            p.handshake_ok = false;
    }
}

std::vector<std::vector<std::uint8_t>> RoomSession::TakeOutgoing()
{
    auto out = std::move(m_out);
    m_out.clear();
    return out;
}
} // namespace phos::p2p


