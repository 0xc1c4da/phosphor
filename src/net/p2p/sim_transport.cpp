#include "sim_transport.h"

namespace phos::p2p
{
void SimTransport::RegisterPeer(std::string peer_id, OnMessageFn on_msg)
{
    m_peers[std::move(peer_id)].on_msg = std::move(on_msg);
}

void SimTransport::Subscribe(std::string peer_id, std::string topic)
{
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end())
        return;
    it->second.subs[std::move(topic)] = true;
}

void SimTransport::Broadcast(const std::string& from_peer_id, const std::string& topic, const std::vector<std::uint8_t>& bytes)
{
    for (auto& [pid, peer] : m_peers)
    {
        if (!peer.on_msg)
            continue;
        if (peer.subs.find(topic) == peer.subs.end())
            continue;
        peer.on_msg(SimMessage{from_peer_id, topic, bytes});
    }
}
} // namespace phos::p2p


