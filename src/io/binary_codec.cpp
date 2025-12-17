#include "io/binary_codec.h"

#include <zstd.h>

#include <limits>
#include <vector>

static inline int B64Index(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64Encode(const std::uint8_t* data, size_t len, std::string& out)
{
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    if (!data && len != 0)
        return false;

    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        const std::uint32_t a = data[i];
        const std::uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        const std::uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        out.push_back(kTbl[(triple >> 18) & 0x3F]);
        out.push_back(kTbl[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTbl[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTbl[(triple >> 0) & 0x3F] : '=');
    }
    return true;
}

bool Base64Decode(const std::string& b64, std::string& out_bytes)
{
    out_bytes.clear();
    if (b64.empty())
        return true;

    // Ignore whitespace.
    std::string s;
    s.reserve(b64.size());
    for (unsigned char c : b64)
    {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
            continue;
        s.push_back((char)c);
    }
    if (s.size() % 4 != 0)
        return false;

    out_bytes.reserve((s.size() / 4) * 3);
    for (size_t i = 0; i < s.size(); i += 4)
    {
        const bool last_quad = (i + 4 == s.size());

        const char c0 = s[i + 0];
        const char c1 = s[i + 1];
        const char c2 = s[i + 2];
        const char c3 = s[i + 3];

        // '=' padding is only legal in the final 1-2 chars of the final quartet.
        if (c0 == '=' || c1 == '=')
            return false;
        if ((c2 == '=' || c3 == '=') && !last_quad)
            return false;
        if (c2 == '=' && c3 != '=')
            return false;

        int v0 = B64Index((unsigned char)c0);
        int v1 = B64Index((unsigned char)c1);
        int v2 = (c2 == '=') ? 0 : B64Index((unsigned char)c2);
        int v3 = (c3 == '=') ? 0 : B64Index((unsigned char)c3);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
            return false;

        std::uint32_t triple = ((std::uint32_t)v0 << 18) |
                               ((std::uint32_t)v1 << 12) |
                               ((std::uint32_t)v2 << 6)  |
                               ((std::uint32_t)v3);

        out_bytes.push_back((char)((triple >> 16) & 0xFF));
        if (c2 != '=')
            out_bytes.push_back((char)((triple >> 8) & 0xFF));
        if (c3 != '=')
            out_bytes.push_back((char)((triple >> 0) & 0xFF));
    }
    return true;
}

bool ZstdCompressBytes(const std::string& in, std::string& out, std::string& err)
{
    err.clear();
    out.clear();

    const size_t bound = ZSTD_compressBound(in.size());
    std::vector<std::uint8_t> buf(bound);

    const int level = 3;
    const size_t n = ZSTD_compress(buf.data(), buf.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n))
    {
        err = std::string("zstd compress failed: ") + ZSTD_getErrorName(n);
        return false;
    }

    out.assign(reinterpret_cast<const char*>(buf.data()), reinterpret_cast<const char*>(buf.data() + n));
    return true;
}

bool ZstdDecompressBytesKnownSize(const std::string& in, std::uint64_t out_size, std::string& out, std::string& err)
{
    err.clear();
    out.clear();

    // Hard safety cap: session files are user-controlled input, and we always know the size we
    // intend to decompress to. Still, bound allocation to avoid OOM / abuse.
    constexpr std::uint64_t kMaxOutSize = (1ull << 30); // 1 GiB
    if (out_size > kMaxOutSize)
    {
        err = "zstd decompress failed: requested output size exceeds 1GiB limit.";
        return false;
    }

    if (out_size > (std::uint64_t)std::numeric_limits<size_t>::max())
    {
        err = "zstd decompress failed: output size too large for this platform.";
        return false;
    }

    out.resize((size_t)out_size);
    const size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n))
    {
        err = std::string("zstd decompress failed: ") + ZSTD_getErrorName(n);
        out.clear();
        return false;
    }
    if (n != out.size())
    {
        err = "zstd decompress failed: size mismatch.";
        out.clear();
        return false;
    }
    return true;
}


