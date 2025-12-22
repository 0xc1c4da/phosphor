#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace http
{
enum class CacheMode
{
    Default = 0,   // read cache if present, otherwise network; write successful responses to cache
    CacheOnly,     // never hit the network; return cached bytes if present
    NetworkOnly,   // always hit the network (still writes to cache on success)
};

struct Response
{
    long status = 0;
    std::vector<std::uint8_t> body;
    std::string err;
    bool from_cache = false; // true when served from disk cache
    bool changed = false;    // for NetworkOnly/Default: true when network response differs from existing cached bytes (or no cache)
};

// Simple blocking GET (HTTPS supported via libcurl).
// - Follows redirects
// - Sets a basic User-Agent
// - Returns status code + raw body bytes
Response Get(const std::string& url,
             const std::map<std::string, std::string>& headers = {},
             CacheMode cache_mode = CacheMode::Default);

inline bool Ok(const Response& r) { return r.err.empty() && r.status >= 200 && r.status < 300; }

// Returns true if this URL (with these headers) is already present in the on-disk cache.
// Never hits the network.
bool HasCached(const std::string& url,
               const std::map<std::string, std::string>& headers = {});
} // namespace http


