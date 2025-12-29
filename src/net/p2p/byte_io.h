// Small byte IO helpers for Phosphor P2P wire formats.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace phos::p2p
{
inline void AppendU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

inline void AppendBytes(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void AppendU16LE(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

inline void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

inline void AppendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

inline std::uint8_t ReadU8(std::span<const std::uint8_t> in, std::size_t& off)
{
    if (off + 1 > in.size())
        throw std::runtime_error("ReadU8: out of range");
    return in[off++];
}

inline std::uint16_t ReadU16LE(std::span<const std::uint8_t> in, std::size_t& off)
{
    if (off + 2 > in.size())
        throw std::runtime_error("ReadU16LE: out of range");
    std::uint16_t v = static_cast<std::uint16_t>(in[off]) | (static_cast<std::uint16_t>(in[off + 1]) << 8);
    off += 2;
    return v;
}

inline std::uint32_t ReadU32LE(std::span<const std::uint8_t> in, std::size_t& off)
{
    if (off + 4 > in.size())
        throw std::runtime_error("ReadU32LE: out of range");
    std::uint32_t v = static_cast<std::uint32_t>(in[off]) | (static_cast<std::uint32_t>(in[off + 1]) << 8) |
                      (static_cast<std::uint32_t>(in[off + 2]) << 16) |
                      (static_cast<std::uint32_t>(in[off + 3]) << 24);
    off += 4;
    return v;
}

inline std::uint64_t ReadU64LE(std::span<const std::uint8_t> in, std::size_t& off)
{
    if (off + 8 > in.size())
        throw std::runtime_error("ReadU64LE: out of range");
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (static_cast<std::uint64_t>(in[off + i]) << (8 * i));
    off += 8;
    return v;
}

inline std::span<const std::uint8_t> ReadBytes(std::span<const std::uint8_t> in, std::size_t& off, std::size_t n)
{
    if (off + n > in.size())
        throw std::runtime_error("ReadBytes: out of range");
    auto s = in.subspan(off, n);
    off += n;
    return s;
}
} // namespace phos::p2p


