// Thin C++ wrapper around automerge-c for sync testing and P2P session logic.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// automerge-c headers
#include <automerge-c/automerge.h>

namespace phos::p2p
{
// NOTE: The installed automerge-c header in this environment does not declare AMresultFree(),
// but the symbol exists in the library. Declare it here to manage lifetimes.
extern "C" void AMresultFree(struct AMresult* result);

struct AutomergeDoc
{
    // The AMdoc pointer is owned by doc_result; do not free separately.
    AMresult* doc_result = nullptr;
    AMdoc* doc = nullptr;

    AutomergeDoc() = default;
    AutomergeDoc(const AutomergeDoc&) = delete;
    AutomergeDoc& operator=(const AutomergeDoc&) = delete;
    AutomergeDoc(AutomergeDoc&& o) noexcept : doc_result(o.doc_result), doc(o.doc)
    {
        o.doc_result = nullptr;
        o.doc = nullptr;
    }
    AutomergeDoc& operator=(AutomergeDoc&& o) noexcept
    {
        if (this == &o)
            return *this;
        Reset();
        doc_result = o.doc_result;
        doc = o.doc;
        o.doc_result = nullptr;
        o.doc = nullptr;
        return *this;
    }
    ~AutomergeDoc() { Reset(); }

    void Reset()
    {
        if (doc_result)
        {
            AMresultFree(doc_result);
            doc_result = nullptr;
            doc = nullptr;
        }
    }

    explicit operator bool() const { return doc != nullptr; }
};

class AutomergeAdapter
{
public:
    AutomergeAdapter() = default;

    // Create a new doc and set its actor id to the provided bytes (deterministic).
    bool CreateWithActorId(std::span<const std::uint8_t> actor_id_bytes, std::string* err = nullptr);

    bool Valid() const { return m_doc.doc != nullptr; }
    AMdoc* Raw() { return m_doc.doc; }
    const AMdoc* Raw() const { return m_doc.doc; }

    // Simple mutation helper for tests: root["key"] = int64.
    bool RootPutInt(std::string_view key, std::int64_t value, std::string* err = nullptr);
    bool Commit(std::string_view message = {}, std::string* err = nullptr);

    // Save/load.
    bool Save(std::vector<std::uint8_t>& out_bytes, std::string* err = nullptr);
    bool LoadFromBytes(std::span<const std::uint8_t> bytes, std::string* err = nullptr);

    // Sync state is tracked per peer key (arbitrary string).
    std::optional<std::vector<std::uint8_t>> GenerateSync(std::string_view peer_key, std::string* err = nullptr);
    bool ReceiveSync(std::string_view peer_key, std::span<const std::uint8_t> sync_bytes, std::string* err = nullptr);

    // Compare docs for equality.
    bool Equal(const AutomergeAdapter& other) const;

private:
    struct SyncState
    {
        AMresult* state_result = nullptr;
        AMsyncState* state = nullptr;
        ~SyncState()
        {
            if (state_result)
                AMresultFree(state_result);
        }
        SyncState() = default;
        SyncState(const SyncState&) = delete;
        SyncState& operator=(const SyncState&) = delete;
        SyncState(SyncState&& o) noexcept : state_result(o.state_result), state(o.state)
        {
            o.state_result = nullptr;
            o.state = nullptr;
        }
        SyncState& operator=(SyncState&& o) noexcept
        {
            if (this == &o)
                return *this;
            if (state_result)
                AMresultFree(state_result);
            state_result = o.state_result;
            state = o.state;
            o.state_result = nullptr;
            o.state = nullptr;
            return *this;
        }
    };

    SyncState& GetOrCreateSyncState(std::string_view peer_key, std::string* err);

    AutomergeDoc m_doc;
    std::unordered_map<std::string, SyncState> m_sync;
};
} // namespace phos::p2p


