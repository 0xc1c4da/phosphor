// Secp256k1 helpers for PHOS wire signatures (compact 64-byte ECDSA).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phos::p2p
{
// Signs message bytes using a 32-byte secp256k1 private key.
// Returns compact 64-byte signature (r||s) with low-S normalization.
bool SecpSignCompact(std::span<const std::uint8_t> privkey32, std::span<const std::uint8_t> msg32,
                     std::array<std::uint8_t, 64>& out_sig, std::string* err = nullptr);

// Verifies compact 64-byte signature over msg32 against compressed 33-byte pubkey.
bool SecpVerifyCompact(std::span<const std::uint8_t> pubkey33, std::span<const std::uint8_t> msg32,
                       std::span<const std::uint8_t> sig64, std::string* err = nullptr);

// Derives compressed pubkey33 from privkey32.
bool SecpPubkeyFromPriv(std::span<const std::uint8_t> privkey32, std::array<std::uint8_t, 33>& out_pubkey33,
                        std::string* err = nullptr);
} // namespace phos::p2p


