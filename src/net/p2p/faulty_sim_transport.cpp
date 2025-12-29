#include "faulty_sim_transport.h"

#include <algorithm>

namespace phos::p2p
{
FaultySimTransport::FaultySimTransport(FaultySimConfig cfg) : m_cfg(std::move(cfg))
{
    m_rng = (m_cfg.seed == 0) ? 1 : m_cfg.seed;
}

void FaultySimTransport::RegisterPeer(std::string peer_id, OnMessageFn on_msg)
{
    m_peers[std::move(peer_id)].on_msg = std::move(on_msg);
}

void FaultySimTransport::Subscribe(std::string peer_id, std::string topic)
{
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end())
        return;
    it->second.subs[std::move(topic)] = true;
}

std::uint64_t FaultySimTransport::NextU64()
{
    // xorshift64*
    std::uint64_t x = m_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    m_rng = x;
    return x * 2685821657736338717ull;
}

std::uint32_t FaultySimTransport::NextU32() { return static_cast<std::uint32_t>(NextU64() & 0xFFFFFFFFu); }

double FaultySimTransport::Next01()
{
    // [0,1)
    const std::uint64_t x = NextU64();
    const std::uint64_t mant = (x >> 11) & ((1ull << 53) - 1);
    return static_cast<double>(mant) / static_cast<double>(1ull << 53);
}

std::uint32_t FaultySimTransport::RandDelayMs()
{
    if (m_cfg.max_delay_ms == 0)
        return 0;
    return static_cast<std::uint32_t>(NextU32() % (m_cfg.max_delay_ms + 1));
}

void FaultySimTransport::Shuffle(std::vector<Pending>& v)
{
    // Fisher-Yates with deterministic RNG.
    for (std::size_t i = v.size(); i > 1; --i)
    {
        const std::size_t j = static_cast<std::size_t>(NextU64() % i);
        std::swap(v[i - 1], v[j]);
    }
}

void FaultySimTransport::Broadcast(const std::string& from_peer_id, const std::string& topic,
                                   const std::vector<std::uint8_t>& bytes, std::uint64_t now_ms)
{
    if (m_pending.size() >= m_cfg.max_pending)
        return;

    for (auto& [pid, peer] : m_peers)
    {
        if (!peer.on_msg)
            continue;
        if (peer.subs.find(topic) == peer.subs.end())
            continue;

        auto schedule_one = [&](const std::string& to_pid) {
            if (m_pending.size() >= m_cfg.max_pending)
                return;
            const std::uint32_t d = RandDelayMs();
            Pending p;
            p.deliver_at_ms = now_ms + d;
            p.seq = ++m_seq;
            p.to_peer_id = to_pid;
            p.msg = SimMessage{from_peer_id, topic, bytes};
            m_pending.push_back(std::move(p));
        };

        // Drop?
        if (m_cfg.drop_prob > 0.0 && Next01() < m_cfg.drop_prob)
            continue;

        schedule_one(pid);

        // Duplicate?
        if (m_cfg.dup_prob > 0.0 && Next01() < m_cfg.dup_prob)
            schedule_one(pid);
    }
}

void FaultySimTransport::Tick(std::uint64_t now_ms)
{
    if (m_pending.empty())
        return;

    std::vector<Pending> due;
    due.reserve(m_pending.size());

    // Partition: keep > now_ms in m_pending, move <= now_ms to due.
    std::vector<Pending> keep;
    keep.reserve(m_pending.size());
    for (auto& p : m_pending)
    {
        if (p.deliver_at_ms <= now_ms)
            due.push_back(std::move(p));
        else
            keep.push_back(std::move(p));
    }
    m_pending = std::move(keep);

    // Reorder all due deliveries deterministically.
    Shuffle(due);

    for (auto& p : due)
    {
        auto it = m_peers.find(p.to_peer_id);
        if (it == m_peers.end())
            continue;
        if (!it->second.on_msg)
            continue;
        it->second.on_msg(p.msg);
    }
}
} // namespace phos::p2p


