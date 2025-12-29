#include "test_harness.h"

#include "net/p2p/blake3_util.h"
#include "net/p2p/wire_frame_v1.h"

#include <cstring>

PHOS_TEST(wire_frame_encode_decode_roundtrip)
{
    using namespace phos::p2p;

    FrameHeaderV1 h;
    h.version = kPhosWireVersion;
    h.type = MsgType::Hello;
    h.flags = PHOS_F_SIGNED; // enc=0 comp=0 no chunk
    h.enc = 0;
    h.comp = 0;
    // fake room tag
    auto rt = Blake3_32("room");
    std::memcpy(h.room_tag.data(), rt.data(), h.room_tag.size());
    h.t_ms = 123456789;
    h.to_tag = 0;
    h.from_tag = 0x1111;
    h.user_tag = 0x2222;
    for (std::size_t i = 0; i < h.msg_id.size(); ++i)
        h.msg_id[i] = static_cast<std::uint8_t>(i);

    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    std::array<std::uint8_t, 64> sig{};
    for (std::size_t i = 0; i < sig.size(); ++i)
        sig[i] = static_cast<std::uint8_t>(0xA0 + (i & 0x0F));

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeFrameV1(h, std::nullopt, std::nullopt, {}, payload, sig, bytes, &err));
    PHOS_REQUIRE_MSG(!bytes.empty(), err);

    ParsedFrameV1 pf;
    PHOS_REQUIRE(DecodeFrameV1(bytes, pf, &err));
    PHOS_REQUIRE_MSG(pf.payload == payload, err);
    PHOS_REQUIRE(pf.hdr.type == MsgType::Hello);
    PHOS_REQUIRE(pf.hdr.from_tag == h.from_tag);
    PHOS_REQUIRE(pf.hdr.user_tag == h.user_tag);
    PHOS_REQUIRE(pf.hdr.to_tag == h.to_tag);
    PHOS_REQUIRE(std::memcmp(pf.sig.data(), sig.data(), 64) == 0);

    // Recompute signed bytes from the decoded frame and ensure they match the originally signed bytes.
    std::vector<std::uint8_t> signed2;
    PHOS_REQUIRE(BuildSignedBytesV1(pf.hdr, pf.key_id, pf.chunk, pf.nonce, pf.payload, signed2, &err));
    PHOS_REQUIRE(bytes.size() >= 64);
    const std::vector<std::uint8_t> signed1(bytes.begin(), bytes.end() - 64);
    PHOS_REQUIRE_MSG(signed1 == signed2, err);
}

PHOS_TEST(wire_frame_decode_rejects_bad_magic)
{
    using namespace phos::p2p;

    FrameHeaderV1 h;
    h.version = kPhosWireVersion;
    h.type = MsgType::Hello;
    h.flags = PHOS_F_SIGNED;
    h.enc = 0;
    h.comp = 0;
    auto rt = Blake3_32("room");
    std::memcpy(h.room_tag.data(), rt.data(), h.room_tag.size());
    h.t_ms = 1;
    h.from_tag = 1;
    h.user_tag = 2;
    h.msg_id = {};

    const std::vector<std::uint8_t> payload = {1, 2, 3};
    std::array<std::uint8_t, 64> sig{};

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeFrameV1(h, std::nullopt, std::nullopt, {}, payload, sig, bytes, &err));
    PHOS_REQUIRE(bytes.size() > 4);

    bytes[0] = 'X';
    ParsedFrameV1 pf;
    PHOS_REQUIRE(!DecodeFrameV1(bytes, pf, &err));
}

PHOS_TEST(wire_frame_decode_rejects_body_len_mismatch)
{
    using namespace phos::p2p;

    FrameHeaderV1 h;
    h.version = kPhosWireVersion;
    h.type = MsgType::Hello;
    h.flags = PHOS_F_SIGNED;
    h.enc = 0;
    h.comp = 0;
    auto rt = Blake3_32("room");
    std::memcpy(h.room_tag.data(), rt.data(), h.room_tag.size());
    h.t_ms = 1;
    h.from_tag = 1;
    h.user_tag = 2;

    const std::vector<std::uint8_t> payload = {1, 2, 3};
    std::array<std::uint8_t, 64> sig{};

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeFrameV1(h, std::nullopt, std::nullopt, {}, payload, sig, bytes, &err));
    PHOS_REQUIRE(bytes.size() >= 4);

    // Corrupt the on-wire body_len (last 4 bytes of the fixed-size header).
    // HeaderBytesV1 = 4+1+1+2+1+1+20+8+8+8+8+16+4 = 82, so body_len starts at 78.
    const std::size_t body_len_off = 78;
    PHOS_REQUIRE(body_len_off + 4 <= bytes.size());
    bytes[body_len_off + 0] ^= 0xFF;

    ParsedFrameV1 pf;
    PHOS_REQUIRE(!DecodeFrameV1(bytes, pf, &err));
}

PHOS_TEST(wire_frame_decode_rejects_truncated_signature)
{
    using namespace phos::p2p;

    FrameHeaderV1 h;
    h.version = kPhosWireVersion;
    h.type = MsgType::Hello;
    h.flags = PHOS_F_SIGNED;
    h.enc = 0;
    h.comp = 0;
    auto rt = Blake3_32("room");
    std::memcpy(h.room_tag.data(), rt.data(), h.room_tag.size());
    h.t_ms = 1;
    h.from_tag = 1;
    h.user_tag = 2;

    const std::vector<std::uint8_t> payload = {1, 2, 3};
    std::array<std::uint8_t, 64> sig{};

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeFrameV1(h, std::nullopt, std::nullopt, {}, payload, sig, bytes, &err));
    PHOS_REQUIRE(bytes.size() > 1);
    bytes.pop_back();

    ParsedFrameV1 pf;
    PHOS_REQUIRE(!DecodeFrameV1(bytes, pf, &err));
}

PHOS_TEST(wire_frame_decode_rejects_unknown_enc_suite)
{
    using namespace phos::p2p;

    FrameHeaderV1 h;
    h.version = kPhosWireVersion;
    h.type = MsgType::Hello;
    h.flags = PHOS_F_SIGNED | PHOS_F_ENCRYPTED;
    h.enc = 9; // unknown suite
    h.comp = 0;
    auto rt = Blake3_32("room");
    std::memcpy(h.room_tag.data(), rt.data(), h.room_tag.size());
    h.t_ms = 1;
    h.from_tag = 1;
    h.user_tag = 2;

    const std::vector<std::uint8_t> payload = {1, 2, 3};
    const std::vector<std::uint8_t> nonce = {0, 1, 2, 3}; // arbitrary; decoder will reject suite before using it
    std::array<std::uint8_t, 64> sig{};

    std::vector<std::uint8_t> bytes;
    std::string err;
    PHOS_REQUIRE(EncodeFrameV1(h, std::nullopt, std::nullopt, nonce, payload, sig, bytes, &err));

    ParsedFrameV1 pf;
    PHOS_REQUIRE(!DecodeFrameV1(bytes, pf, &err));
}


