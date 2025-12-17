#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Generic helpers for storing binary blobs in JSON:
// - base64 encode/decode
// - zstd compress/decompress (in-memory)
//
// Used by session restore (session.json) to store CBOR payloads compactly, but not
// inherently "session-specific".
bool Base64Encode(const std::uint8_t* data, size_t len, std::string& out);
bool Base64Decode(const std::string& b64, std::string& out_bytes);

bool ZstdCompressBytes(const std::string& in, std::string& out, std::string& err);
bool ZstdDecompressBytesKnownSize(const std::string& in, std::uint64_t out_size, std::string& out, std::string& err);


