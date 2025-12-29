// PHOS binary wire framing v1 (see references/networking/canvas-p2p-multiplayer.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phos::p2p
{
inline constexpr std::array<std::uint8_t, 4> kPhosMagic{{'P', 'H', 'O', 'S'}};
inline constexpr std::uint8_t kPhosWireVersion = 1;

enum class MsgType : std::uint8_t
{
    Hello = 1,
    Sync = 2,
    SyncChunk = 3,
    Presence = 4,
    SnapshotReq = 5,
    SnapshotChunk = 6,
    Ping = 7,
    Pong = 8,
};

// Header flags (u16).
enum : std::uint16_t
{
    PHOS_F_ENCRYPTED = 1u << 0,
    PHOS_F_SIGNED = 1u << 1,
    PHOS_F_COMPRESSED = 1u << 2,
    PHOS_F_CHUNKED = 1u << 3,
    PHOS_F_HAS_KEY_ID = 1u << 4,
};

struct FrameHeaderV1
{
    std::uint8_t version = kPhosWireVersion;
    MsgType type = MsgType::Hello;
    std::uint16_t flags = PHOS_F_SIGNED;
    std::uint8_t enc = 0;
    std::uint8_t comp = 0;
    std::array<std::uint8_t, 20> room_tag{};
    std::uint64_t t_ms = 0;
    std::uint64_t to_tag = 0;
    std::uint64_t from_tag = 0;
    std::uint64_t user_tag = 0;
    std::array<std::uint8_t, 16> msg_id{};
    std::uint32_t body_len = 0; // computed at encode time
};

struct ChunkMetaV1
{
    std::array<std::uint8_t, 16> xfer_id{};
    std::uint32_t chunk_index = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t full_len = 0;
};

struct ParsedFrameV1
{
    FrameHeaderV1 hdr;
    std::optional<std::uint32_t> key_id;
    std::optional<ChunkMetaV1> chunk;
    std::vector<std::uint8_t> nonce;   // present iff enc!=0
    std::vector<std::uint8_t> payload; // plaintext if enc==0 else ciphertext
    std::array<std::uint8_t, 64> sig{};
};

// Serialize header+body (including signature) into bytes.
// This is a low-level framing function; it does not encrypt/compress.
bool EncodeFrameV1(const FrameHeaderV1& hdr_in, const std::optional<std::uint32_t>& key_id,
                   const std::optional<ChunkMetaV1>& chunk, std::span<const std::uint8_t> nonce,
                   std::span<const std::uint8_t> payload, std::span<const std::uint8_t> sig64,
                   std::vector<std::uint8_t>& out, std::string* err = nullptr);

// Parses and validates the framing shape (magic/version/lengths). Does not verify signatures.
bool DecodeFrameV1(std::span<const std::uint8_t> bytes, ParsedFrameV1& out, std::string* err = nullptr);

// Returns the bytes that must be signed/verified for this frame: header bytes + body bytes excluding signature.
// (Only valid after EncodeFrameV1 or after DecodeFrameV1 reconstructs the bytes.)
bool BuildSignedBytesV1(const FrameHeaderV1& hdr, const std::optional<std::uint32_t>& key_id,
                        const std::optional<ChunkMetaV1>& chunk, std::span<const std::uint8_t> nonce,
                        std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out,
                        std::string* err = nullptr);
} // namespace phos::p2p


