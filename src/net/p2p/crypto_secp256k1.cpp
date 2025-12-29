#include "crypto_secp256k1.h"

#include <secp256k1.h>

namespace phos::p2p
{
namespace
{
secp256k1_context* CtxSign()
{
    static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    return ctx;
}
secp256k1_context* CtxVerify()
{
    static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    return ctx;
}
} // namespace

bool SecpPubkeyFromPriv(std::span<const std::uint8_t> privkey32, std::array<std::uint8_t, 33>& out_pubkey33,
                        std::string* err)
{
    if (privkey32.size() != 32)
    {
        if (err)
            *err = "SecpPubkeyFromPriv: privkey must be 32 bytes";
        return false;
    }

    secp256k1_pubkey pub{};
    if (!secp256k1_ec_pubkey_create(CtxSign(), &pub, privkey32.data()))
    {
        if (err)
            *err = "SecpPubkeyFromPriv: secp256k1_ec_pubkey_create failed";
        return false;
    }

    size_t outlen = out_pubkey33.size();
    if (!secp256k1_ec_pubkey_serialize(CtxSign(), out_pubkey33.data(), &outlen, &pub, SECP256K1_EC_COMPRESSED) ||
        outlen != out_pubkey33.size())
    {
        if (err)
            *err = "SecpPubkeyFromPriv: serialize failed";
        return false;
    }
    return true;
}

bool SecpSignCompact(std::span<const std::uint8_t> privkey32, std::span<const std::uint8_t> msg32,
                     std::array<std::uint8_t, 64>& out_sig, std::string* err)
{
    if (privkey32.size() != 32 || msg32.size() != 32)
    {
        if (err)
            *err = "SecpSignCompact: privkey and msg must be 32 bytes";
        return false;
    }

    secp256k1_ecdsa_signature sig{};
    if (!secp256k1_ecdsa_sign(CtxSign(), &sig, msg32.data(), privkey32.data(), nullptr, nullptr))
    {
        if (err)
            *err = "SecpSignCompact: secp256k1_ecdsa_sign failed";
        return false;
    }

    // Enforce low-S (normalization makes sig canonical).
    secp256k1_ecdsa_signature norm{};
    secp256k1_ecdsa_signature_normalize(CtxSign(), &norm, &sig);

    if (!secp256k1_ecdsa_signature_serialize_compact(CtxSign(), out_sig.data(), &norm))
    {
        if (err)
            *err = "SecpSignCompact: serialize_compact failed";
        return false;
    }
    return true;
}

bool SecpVerifyCompact(std::span<const std::uint8_t> pubkey33, std::span<const std::uint8_t> msg32,
                       std::span<const std::uint8_t> sig64, std::string* err)
{
    if (pubkey33.size() != 33 || msg32.size() != 32 || sig64.size() != 64)
    {
        if (err)
            *err = "SecpVerifyCompact: pubkey33/msg32/sig64 sizes invalid";
        return false;
    }

    secp256k1_pubkey pub{};
    if (!secp256k1_ec_pubkey_parse(CtxVerify(), &pub, pubkey33.data(), pubkey33.size()))
    {
        if (err)
            *err = "SecpVerifyCompact: pubkey parse failed";
        return false;
    }

    secp256k1_ecdsa_signature sig{};
    if (!secp256k1_ecdsa_signature_parse_compact(CtxVerify(), &sig, sig64.data()))
    {
        if (err)
            *err = "SecpVerifyCompact: signature parse failed";
        return false;
    }

    // Accept both normalized and non-normalized signatures, but verify against normalized.
    secp256k1_ecdsa_signature norm{};
    secp256k1_ecdsa_signature_normalize(CtxVerify(), &norm, &sig);

    const int ok = secp256k1_ecdsa_verify(CtxVerify(), &norm, msg32.data(), &pub);
    if (!ok && err)
        *err = "SecpVerifyCompact: verify failed";
    return ok != 0;
}
} // namespace phos::p2p


