// Headless P2P "room session" state machine (Phase 1).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "automerge_adapter.h"
#include "chunk_reassembly.h"

namespace phos::p2p
{
struct RoomPeer
{
    std::string peer_id;
    std::array<std::uint8_t, 33> pubkey33{};
    std::string name;
    std::uint64_t from_tag = 0;
    std::uint64_t user_tag = 0;
    std::uint64_t last_seen_ms = 0;

    bool handshake_ok = false;
};

struct RoomSessionConfig
{
    std::string topic;
    std::string local_peer_id;
    std::string display_name = "anon";

    // v1: signing identity (required)
    std::array<std::uint8_t, 32> privkey32{};

    // Timers (ms).
    std::uint64_t hello_interval_ms = 2000;
    std::uint64_t sync_interval_ms = 200;

    // Hard upper bound for wire messages. If a sync payload exceeds this, it will be chunked.
    std::size_t max_wire_bytes = 256 * 1024;

    ChunkCaps chunk_caps{};
};

class RoomSession
{
public:
    explicit RoomSession(RoomSessionConfig cfg);

    bool Init(std::string* err = nullptr);

    const std::string& Topic() const { return m_cfg.topic; }
    const std::string& LocalPeerId() const { return m_cfg.local_peer_id; }

    std::uint64_t MyToTag() const { return m_my_to_tag; }
    std::uint64_t MyFromTag() const { return m_my_from_tag; }
    std::uint64_t MyUserTag() const { return m_my_user_tag; }
    const std::array<std::uint8_t, 20>& RoomTag() const { return m_room_tag; }

    AutomergeAdapter& Doc() { return m_doc; }
    const AutomergeAdapter& Doc() const { return m_doc; }

    const std::unordered_map<std::string, RoomPeer>& Peers() const { return m_peers; }

    // Ingest a raw wire frame received from the room topic.
    // `now_ms` is the receiver's local monotonic time (ms) used for liveness tracking.
    void OnIncomingBytes(std::string_view from_peer_id, std::span<const std::uint8_t> bytes, std::uint64_t now_ms);

    // Drives periodic HELLO broadcasts, sync generation, stale peer detection, etc.
    void Tick(std::uint64_t now_ms);

    // Returns queued outgoing frames and clears the queue.
    std::vector<std::vector<std::uint8_t>> TakeOutgoing();

private:
    RoomSessionConfig m_cfg;

    std::array<std::uint8_t, 20> m_room_tag{};
    std::uint64_t m_my_to_tag = 0;
    std::uint64_t m_my_from_tag = 0;
    std::array<std::uint8_t, 33> m_my_pubkey33{};
    std::uint64_t m_my_user_tag = 0;

    AutomergeAdapter m_doc;
    std::unordered_map<std::string, RoomPeer> m_peers;

    ChunkReassembler m_reasm;

    std::uint64_t m_last_hello_ms = 0;
    std::uint64_t m_last_sync_ms = 0;

    std::vector<std::vector<std::uint8_t>> m_out;

    void Enqueue(std::vector<std::uint8_t> frame_bytes);
    void EnqueueHello(std::uint64_t now_ms);
    void EnqueueSyncToAll(std::uint64_t now_ms);
    void EnqueueSyncToPeer(std::string_view peer_id, std::span<const std::uint8_t> sync_bytes, std::uint64_t now_ms);

    void OnHello(std::string_view from_peer_id, std::uint64_t now_ms, std::span<const std::uint8_t> payload,
                 std::uint64_t from_tag, std::uint64_t user_tag);
    void OnSync(std::string_view from_peer_id, std::span<const std::uint8_t> sync_bytes, std::uint64_t now_ms);
    void OnSyncChunk(std::string_view from_peer_id, std::uint64_t from_tag, std::span<const std::uint8_t> chunk_bytes,
                     const std::array<std::uint8_t, 16>& xfer_id, std::uint32_t chunk_index, std::uint32_t chunk_count,
                     std::uint32_t full_len, std::uint64_t now_ms);
};
} // namespace phos::p2p


