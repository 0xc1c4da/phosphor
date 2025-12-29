// Deterministic in-memory "transport" for headless multiplayer tests/lab.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace phos::p2p
{
struct SimMessage
{
    std::string from_peer_id;
    std::string topic;
    std::vector<std::uint8_t> bytes;
};

class SimTransport
{
public:
    using OnMessageFn = std::function<void(const SimMessage&)>;

    void RegisterPeer(std::string peer_id, OnMessageFn on_msg);

    void Subscribe(std::string peer_id, std::string topic);

    // Broadcast to all subscribers (including sender), like gossipsub.
    void Broadcast(const std::string& from_peer_id, const std::string& topic, const std::vector<std::uint8_t>& bytes);

private:
    struct Peer
    {
        OnMessageFn on_msg;
        std::unordered_map<std::string, bool> subs;
    };
    std::unordered_map<std::string, Peer> m_peers;
};
} // namespace phos::p2p


