#include "automerge_adapter.h"

#include <cstring>

namespace phos::p2p
{
namespace
{
inline AMbyteSpan ByteSpan(std::string_view s)
{
    return AMbyteSpan{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

inline bool ResultOk(AMresult* r) { return r != nullptr && AMresultStatus(r) == AM_STATUS_OK; }

inline std::string ResultErrorStr(AMresult* r)
{
    if (!r)
        return "null result";
    if (AMresultStatus(r) == AM_STATUS_OK)
        return {};
    const AMbyteSpan s = ::AMresultError(r);
    return std::string(reinterpret_cast<const char*>(s.src), s.count);
}

inline bool ItemBytes(AMitem* it, std::vector<std::uint8_t>& out)
{
    AMbyteSpan bs{};
    if (!AMitemToBytes(it, &bs))
        return false;
    out.assign(bs.src, bs.src + bs.count);
    return true;
}

inline bool ItemDoc(AMitem* it, AMdoc** out_doc) { return AMitemToDoc(it, out_doc); }

inline bool ItemActor(AMitem* it, const AMactorId** out_actor) { return AMitemToActorId(it, out_actor); }

inline bool ItemSyncMessage(const AMitem* it, const AMsyncMessage** out_msg) { return AMitemToSyncMessage(it, out_msg); }

} // namespace

bool AutomergeAdapter::CreateWithActorId(std::span<const std::uint8_t> actor_id_bytes, std::string* err)
{
    m_doc.Reset();
    m_sync.clear();

    AMstack* stack = nullptr;

    // actor_id = AMactorIdFromBytes(...)
    AMresult* actor_r = AMactorIdFromBytes(actor_id_bytes.data(), actor_id_bytes.size());
    if (!ResultOk(actor_r))
    {
        if (err)
            *err = "AMactorIdFromBytes failed: " + ResultErrorStr(actor_r);
        if (actor_r)
            AMresultFree(actor_r);
        return false;
    }
    AMitem* actor_it = AMstackItem(&stack, actor_r, nullptr, nullptr);
    const AMactorId* actor_id = nullptr;
    if (!actor_it || !ItemActor(actor_it, &actor_id) || !actor_id)
    {
        if (err)
            *err = "CreateWithActorId: failed to extract actor id";
        AMstackFree(&stack);
        return false;
    }

    // doc = AMcreate(actor_id)
    AMresult* doc_r = AMcreate(actor_id);
    if (!ResultOk(doc_r))
    {
        if (err)
            *err = "AMcreate failed: " + ResultErrorStr(doc_r);
        if (doc_r)
            AMresultFree(doc_r);
        AMstackFree(&stack);
        return false;
    }
    AMitem* doc_it = AMstackItem(&stack, doc_r, nullptr, nullptr);
    AMdoc* doc_ptr = nullptr;
    if (!doc_it || !ItemDoc(doc_it, &doc_ptr) || !doc_ptr)
    {
        if (err)
            *err = "CreateWithActorId: failed to extract doc";
        AMstackFree(&stack);
        return false;
    }

    // NOTE: doc_ptr is owned by doc_r; keep doc_r for lifetime and free on destruction.
    // We must pop doc_r out of the stack to avoid it being freed when we free the stack.
    // The stack API warns pop'd results must be freed with AMresultFree() (which we do in AutomergeDoc).
    AMresult* kept = AMstackPop(&stack, doc_r);
    (void)kept; // kept == doc_r
    AMstackFree(&stack); // frees actor result etc.

    m_doc.doc_result = doc_r;
    m_doc.doc = doc_ptr;
    return true;
}

AutomergeAdapter::SyncState& AutomergeAdapter::GetOrCreateSyncState(std::string_view peer_key, std::string* err)
{
    auto it = m_sync.find(std::string(peer_key));
    if (it != m_sync.end())
        return it->second;

    AMresult* st_r = AMsyncStateInit();
    if (!ResultOk(st_r))
    {
        if (err)
            *err = "AMsyncStateInit failed: " + ResultErrorStr(st_r);
        if (st_r)
            AMresultFree(st_r);
        // Insert a dummy (will crash later if used); caller should check err after call sites.
        return m_sync.emplace(std::string(peer_key), SyncState{}).first->second;
    }

    AMitem* it0 = AMresultItem(st_r);
    AMsyncState* st_ptr = nullptr;
    if (!it0 || !AMitemToSyncState(it0, &st_ptr) || !st_ptr)
    {
        if (err)
            *err = "GetOrCreateSyncState: failed to extract sync state";
        AMresultFree(st_r);
        return m_sync.emplace(std::string(peer_key), SyncState{}).first->second;
    }

    SyncState st;
    st.state_result = st_r;
    st.state = st_ptr;
    return m_sync.emplace(std::string(peer_key), std::move(st)).first->second;
}

std::optional<std::vector<std::uint8_t>> AutomergeAdapter::GenerateSync(std::string_view peer_key, std::string* err)
{
    if (!Valid())
    {
        if (err)
            *err = "GenerateSync: doc not initialized";
        return std::nullopt;
    }
    SyncState& st = GetOrCreateSyncState(peer_key, err);
    if (!st.state)
        return std::nullopt;

    AMresult* gen_r = AMgenerateSyncMessage(m_doc.doc, st.state);
    if (!ResultOk(gen_r))
    {
        if (err)
            *err = "AMgenerateSyncMessage failed: " + ResultErrorStr(gen_r);
        if (gen_r)
            AMresultFree(gen_r);
        return std::nullopt;
    }

    AMitem* it0 = AMresultItem(gen_r);
    const AMsyncMessage* msg = nullptr;
    if (!it0 || !ItemSyncMessage(it0, &msg) || !msg)
    {
        // No sync message to send (VOID).
        AMresultFree(gen_r);
        return std::nullopt;
    }

    AMresult* enc_r = AMsyncMessageEncode(msg);
    if (!ResultOk(enc_r))
    {
        if (err)
            *err = "AMsyncMessageEncode failed: " + ResultErrorStr(enc_r);
        if (enc_r)
            AMresultFree(enc_r);
        AMresultFree(gen_r);
        return std::nullopt;
    }

    AMitem* enc_it = AMresultItem(enc_r);
    std::vector<std::uint8_t> out_bytes;
    if (!enc_it || !ItemBytes(enc_it, out_bytes))
    {
        if (err)
            *err = "GenerateSync: failed to extract encoded bytes";
        AMresultFree(enc_r);
        AMresultFree(gen_r);
        return std::nullopt;
    }

    AMresultFree(enc_r);
    AMresultFree(gen_r);

    // Defensive: some upstream encoders may return an empty byte span for "no-op" messages.
    // We never want to broadcast an empty sync payload because some automerge-c builds can panic
    // on decoding empty input.
    if (out_bytes.empty())
        return std::nullopt;

    return out_bytes;
}

bool AutomergeAdapter::ReceiveSync(std::string_view peer_key, std::span<const std::uint8_t> sync_bytes, std::string* err)
{
    if (!Valid())
    {
        if (err)
            *err = "ReceiveSync: doc not initialized";
        return false;
    }
    SyncState& st = GetOrCreateSyncState(peer_key, err);
    if (!st.state)
        return false;

    if (sync_bytes.empty())
    {
        if (err)
            *err = "ReceiveSync: empty sync_bytes";
        return false;
    }

    AMresult* dec_r = AMsyncMessageDecode(sync_bytes.data(), sync_bytes.size());
    if (!ResultOk(dec_r))
    {
        if (err)
            *err = "AMsyncMessageDecode failed: " + ResultErrorStr(dec_r);
        if (dec_r)
            AMresultFree(dec_r);
        return false;
    }

    AMitem* it0 = AMresultItem(dec_r);
    const AMsyncMessage* msg = nullptr;
    if (!it0 || !ItemSyncMessage(it0, &msg) || !msg)
    {
        if (err)
            *err = "ReceiveSync: decoded message missing";
        AMresultFree(dec_r);
        return false;
    }

    AMresult* rx_r = AMreceiveSyncMessage(m_doc.doc, st.state, msg);
    if (!ResultOk(rx_r))
    {
        if (err)
            *err = "AMreceiveSyncMessage failed: " + ResultErrorStr(rx_r);
        if (rx_r)
            AMresultFree(rx_r);
        AMresultFree(dec_r);
        return false;
    }

    AMresultFree(rx_r);
    AMresultFree(dec_r);
    return true;
}

bool AutomergeAdapter::RootPutInt(std::string_view key, std::int64_t value, std::string* err)
{
    if (!Valid())
    {
        if (err)
            *err = "RootPutInt: doc not initialized";
        return false;
    }
    AMresult* r = AMmapPutInt(m_doc.doc, AM_ROOT, ByteSpan(key), value);
    if (!ResultOk(r))
    {
        if (err)
            *err = "AMmapPutInt failed: " + ResultErrorStr(r);
        if (r)
            AMresultFree(r);
        return false;
    }
    AMresultFree(r);
    return true;
}

bool AutomergeAdapter::Commit(std::string_view message, std::string* err)
{
    if (!Valid())
    {
        if (err)
            *err = "Commit: doc not initialized";
        return false;
    }
    AMresult* r = AMcommit(m_doc.doc, ByteSpan(message), nullptr);
    if (!ResultOk(r))
    {
        if (err)
            *err = "AMcommit failed: " + ResultErrorStr(r);
        if (r)
            AMresultFree(r);
        return false;
    }
    AMresultFree(r);
    return true;
}

bool AutomergeAdapter::Save(std::vector<std::uint8_t>& out_bytes, std::string* err)
{
    if (!Valid())
    {
        if (err)
            *err = "Save: doc not initialized";
        return false;
    }
    AMresult* r = AMsave(m_doc.doc);
    if (!ResultOk(r))
    {
        if (err)
            *err = "AMsave failed: " + ResultErrorStr(r);
        if (r)
            AMresultFree(r);
        return false;
    }
    AMitem* it0 = AMresultItem(r);
    if (!it0 || !ItemBytes(it0, out_bytes))
    {
        if (err)
            *err = "Save: failed to extract bytes";
        AMresultFree(r);
        return false;
    }
    AMresultFree(r);
    return true;
}

bool AutomergeAdapter::LoadFromBytes(std::span<const std::uint8_t> bytes, std::string* err)
{
    m_doc.Reset();
    m_sync.clear();

    AMresult* r = AMload(bytes.data(), bytes.size());
    if (!ResultOk(r))
    {
        if (err)
            *err = "AMload failed: " + ResultErrorStr(r);
        if (r)
            AMresultFree(r);
        return false;
    }
    AMitem* it0 = AMresultItem(r);
    AMdoc* doc_ptr = nullptr;
    if (!it0 || !ItemDoc(it0, &doc_ptr) || !doc_ptr)
    {
        if (err)
            *err = "LoadFromBytes: failed to extract doc";
        AMresultFree(r);
        return false;
    }
    m_doc.doc_result = r;
    m_doc.doc = doc_ptr;
    return true;
}

bool AutomergeAdapter::Equal(const AutomergeAdapter& other) const
{
    if (!Valid() || !other.Valid())
        return false;
    return AMequal(m_doc.doc, const_cast<AMdoc*>(other.m_doc.doc));
}
} // namespace phos::p2p


