#include "wire_frame_v1.h"

#include "byte_io.h"

#include <algorithm>
#include <cstring>

namespace phos::p2p
{
namespace
{
constexpr std::size_t HeaderBytesV1 = 4 /*magic*/ + 1 + 1 + 2 + 1 + 1 + 20 + 8 + 8 + 8 + 8 + 16 + 4;

inline void AppendHeader(std::vector<std::uint8_t>& out, const FrameHeaderV1& h, std::uint32_t body_len)
{
    AppendBytes(out, kPhosMagic);
    AppendU8(out, h.version);
    AppendU8(out, static_cast<std::uint8_t>(h.type));
    AppendU16LE(out, h.flags);
    AppendU8(out, h.enc);
    AppendU8(out, h.comp);
    AppendBytes(out, h.room_tag);
    AppendU64LE(out, h.t_ms);
    AppendU64LE(out, h.to_tag);
    AppendU64LE(out, h.from_tag);
    AppendU64LE(out, h.user_tag);
    AppendBytes(out, h.msg_id);
    AppendU32LE(out, body_len);
}

inline bool HasFlag(std::uint16_t flags, std::uint16_t f) { return (flags & f) != 0; }
} // namespace

bool BuildSignedBytesV1(const FrameHeaderV1& hdr, const std::optional<std::uint32_t>& key_id,
                        const std::optional<ChunkMetaV1>& chunk, std::span<const std::uint8_t> nonce,
                        std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out, std::string* err)
{
    // Compute body length excluding signature: key_id? + chunk? + nonce? + payload.
    std::uint32_t body_wo_sig = 0;
    if (HasFlag(hdr.flags, PHOS_F_HAS_KEY_ID))
        body_wo_sig += 4;
    if (HasFlag(hdr.flags, PHOS_F_CHUNKED))
        body_wo_sig += 16 + 4 + 4 + 4;
    if (hdr.enc != 0)
        body_wo_sig += static_cast<std::uint32_t>(nonce.size());
    body_wo_sig += static_cast<std::uint32_t>(payload.size());

    out.clear();
    out.reserve(HeaderBytesV1 + body_wo_sig);

    // IMPORTANT: on-wire header's body_len includes the signature bytes too.
    // The signature covers: header_bytes (including that body_len field) || body_bytes_without_signature.
    AppendHeader(out, hdr, body_wo_sig + 64);

    if (HasFlag(hdr.flags, PHOS_F_HAS_KEY_ID))
    {
        if (!key_id)
        {
            if (err)
                *err = "BuildSignedBytesV1: PHOS_F_HAS_KEY_ID set but key_id missing";
            return false;
        }
        AppendU32LE(out, *key_id);
    }
    if (HasFlag(hdr.flags, PHOS_F_CHUNKED))
    {
        if (!chunk)
        {
            if (err)
                *err = "BuildSignedBytesV1: PHOS_F_CHUNKED set but chunk meta missing";
            return false;
        }
        AppendBytes(out, chunk->xfer_id);
        AppendU32LE(out, chunk->chunk_index);
        AppendU32LE(out, chunk->chunk_count);
        AppendU32LE(out, chunk->full_len);
    }
    if (hdr.enc != 0)
        AppendBytes(out, nonce);
    AppendBytes(out, payload);
    return true;
}

bool EncodeFrameV1(const FrameHeaderV1& hdr_in, const std::optional<std::uint32_t>& key_id,
                   const std::optional<ChunkMetaV1>& chunk, std::span<const std::uint8_t> nonce,
                   std::span<const std::uint8_t> payload, std::span<const std::uint8_t> sig64,
                   std::vector<std::uint8_t>& out, std::string* err)
{
    if (sig64.size() != 64)
    {
        if (err)
            *err = "EncodeFrameV1: sig must be 64 bytes";
        return false;
    }
    if (hdr_in.version != kPhosWireVersion)
    {
        if (err)
            *err = "EncodeFrameV1: unsupported version";
        return false;
    }
    if (hdr_in.enc != 0 && nonce.empty())
    {
        if (err)
            *err = "EncodeFrameV1: enc!=0 requires nonce bytes";
        return false;
    }

    std::vector<std::uint8_t> signed_bytes;
    if (!BuildSignedBytesV1(hdr_in, key_id, chunk, nonce, payload, signed_bytes, err))
        return false;

    out = signed_bytes;
    AppendBytes(out, sig64);
    return true;
}

bool DecodeFrameV1(std::span<const std::uint8_t> bytes, ParsedFrameV1& out, std::string* err)
{
    if (bytes.size() < HeaderBytesV1)
    {
        if (err)
            *err = "DecodeFrameV1: too short";
        return false;
    }

    std::size_t off = 0;
    try
    {
        auto magic = ReadBytes(bytes, off, 4);
        if (!std::equal(magic.begin(), magic.end(), kPhosMagic.begin()))
        {
            if (err)
                *err = "DecodeFrameV1: bad magic";
            return false;
        }

        FrameHeaderV1 h;
        h.version = ReadU8(bytes, off);
        if (h.version != kPhosWireVersion)
        {
            if (err)
                *err = "DecodeFrameV1: unsupported version";
            return false;
        }
        h.type = static_cast<MsgType>(ReadU8(bytes, off));
        h.flags = ReadU16LE(bytes, off);
        h.enc = ReadU8(bytes, off);
        h.comp = ReadU8(bytes, off);

        auto room = ReadBytes(bytes, off, h.room_tag.size());
        std::memcpy(h.room_tag.data(), room.data(), h.room_tag.size());

        h.t_ms = ReadU64LE(bytes, off);
        h.to_tag = ReadU64LE(bytes, off);
        h.from_tag = ReadU64LE(bytes, off);
        h.user_tag = ReadU64LE(bytes, off);

        auto msg_id = ReadBytes(bytes, off, h.msg_id.size());
        std::memcpy(h.msg_id.data(), msg_id.data(), h.msg_id.size());

        h.body_len = ReadU32LE(bytes, off);
        if (HeaderBytesV1 + h.body_len != bytes.size())
        {
            if (err)
                *err = "DecodeFrameV1: body_len mismatch";
            return false;
        }
        if (h.body_len < 64)
        {
            if (err)
                *err = "DecodeFrameV1: body too short (missing signature)";
            return false;
        }

        std::size_t body_off = off;
        std::size_t body_end = off + h.body_len;

        ParsedFrameV1 pf;
        pf.hdr = h;

        if (HasFlag(h.flags, PHOS_F_HAS_KEY_ID))
            pf.key_id = ReadU32LE(bytes, body_off);

        if (HasFlag(h.flags, PHOS_F_CHUNKED))
        {
            ChunkMetaV1 cm;
            auto xfer = ReadBytes(bytes, body_off, cm.xfer_id.size());
            std::memcpy(cm.xfer_id.data(), xfer.data(), cm.xfer_id.size());
            cm.chunk_index = ReadU32LE(bytes, body_off);
            cm.chunk_count = ReadU32LE(bytes, body_off);
            cm.full_len = ReadU32LE(bytes, body_off);
            pf.chunk = cm;
        }

        if (h.enc != 0)
        {
            // We don't know nonce size from header. For v1, only XChaCha (24) or AES-GCM (12).
            // Heuristic: if PHOS_F_ENCRYPTED set and enc==1 -> 24, enc==2 -> 12.
            std::size_t nonce_len = 0;
            if (h.enc == 1)
                nonce_len = 24;
            else if (h.enc == 2)
                nonce_len = 12;
            else
            {
                if (err)
                    *err = "DecodeFrameV1: unknown enc suite";
                return false;
            }
            auto n = ReadBytes(bytes, body_off, nonce_len);
            pf.nonce.assign(n.begin(), n.end());
        }

        // Remaining bytes: payload + sig64.
        if (body_end < body_off + 64)
        {
            if (err)
                *err = "DecodeFrameV1: invalid body framing";
            return false;
        }
        const std::size_t sig_off = body_end - 64;
        auto payload_span = bytes.subspan(body_off, sig_off - body_off);
        pf.payload.assign(payload_span.begin(), payload_span.end());
        auto sig_span = bytes.subspan(sig_off, 64);
        std::memcpy(pf.sig.data(), sig_span.data(), 64);

        out = std::move(pf);
        return true;
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = std::string("DecodeFrameV1: ") + e.what();
        return false;
    }
}
} // namespace phos::p2p


