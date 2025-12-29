// Deterministic in-memory "transport" with fault injection for headless tests.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim_transport.h"

namespace phos::p2p
{
struct FaultySimConfig
{
    // Seed for deterministic behavior.
    std::uint64_t seed = 1;

    // Probabilities are in [0, 1].
    double drop_prob = 0.0; // drop a delivery
    double dup_prob = 0.0;  // duplicate a delivery (in addition to normal delivery)

    // Adds a random delay in [0, max_delay_ms] per delivery, which induces reordering.
    std::uint32_t max_delay_ms = 0;

    // Hard cap on pending deliveries (DoS protection for tests).
    std::size_t max_pending = 200'000;
};

class FaultySimTransport
{
public:
    using OnMessageFn = SimTransport::OnMessageFn;

    explicit FaultySimTransport(FaultySimConfig cfg);

    void RegisterPeer(std::string peer_id, OnMessageFn on_msg);
    void Subscribe(std::string peer_id, std::string topic);

    // Deterministic partition rules (drop deliveries between two peers during a time window).
    // Semantics:
    // - If now_ms is in [start_ms, end_ms) for a rule, all deliveries A->B and B->A are dropped.
    // - Rules are evaluated at delivery time (Tick), so delayed messages can be dropped if they
    //   land within the window.
    void AddPartition(std::string peer_a, std::string peer_b, std::uint64_t start_ms, std::uint64_t end_ms);

    // Schedule a broadcast to all subscribers (including sender).
    void Broadcast(const std::string& from_peer_id, const std::string& topic, const std::vector<std::uint8_t>& bytes,
                   std::uint64_t now_ms);

    // Deliver all pending messages whose delivery time is <= now_ms.
    void Tick(std::uint64_t now_ms);

private:
    struct Partition
    {
        std::string a;
        std::string b;
        std::uint64_t start_ms = 0;
        std::uint64_t end_ms = 0; // exclusive
    };

    struct Peer
    {
        OnMessageFn on_msg;
        std::unordered_map<std::string, bool> subs;
    };

    struct Pending
    {
        std::uint64_t deliver_at_ms = 0;
        std::uint64_t seq = 0;
        std::string to_peer_id;
        SimMessage msg;
    };

    FaultySimConfig m_cfg;
    std::uint64_t m_seq = 0;
    std::uint64_t m_rng = 1;

    std::unordered_map<std::string, Peer> m_peers;
    std::vector<Pending> m_pending;
    std::vector<Partition> m_partitions;

    std::uint64_t NextU64();
    std::uint32_t NextU32();
    double Next01();
    std::uint32_t RandDelayMs();
    void Shuffle(std::vector<Pending>& v);

    bool IsPartitioned(std::string_view from_peer_id, std::string_view to_peer_id, std::uint64_t now_ms) const;
};
} // namespace phos::p2p


