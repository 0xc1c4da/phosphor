#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace http
{
struct Response
{
    long status = 0;
    std::vector<std::uint8_t> body;
    std::string err;
};

// Simple blocking GET (HTTPS supported via libcurl).
// - Follows redirects
// - Sets a basic User-Agent
// - Returns status code + raw body bytes
Response Get(const std::string& url,
             const std::map<std::string, std::string>& headers = {});

inline bool Ok(const Response& r) { return r.err.empty() && r.status >= 200 && r.status < 300; }
} // namespace http


