#include "binary_codec.h"

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

    size_t pad = 0;
    if (!s.empty() && s.back() == '=') pad++;
    if (s.size() >= 2 && s[s.size() - 2] == '=') pad++;

    out_bytes.reserve((s.size() / 4) * 3);
    for (size_t i = 0; i < s.size(); i += 4)
    {
        int v0 = (s[i + 0] == '=') ? 0 : B64Index((unsigned char)s[i + 0]);
        int v1 = (s[i + 1] == '=') ? 0 : B64Index((unsigned char)s[i + 1]);
        int v2 = (s[i + 2] == '=') ? 0 : B64Index((unsigned char)s[i + 2]);
        int v3 = (s[i + 3] == '=') ? 0 : B64Index((unsigned char)s[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
            return false;

        std::uint32_t triple = ((std::uint32_t)v0 << 18) |
                               ((std::uint32_t)v1 << 12) |
                               ((std::uint32_t)v2 << 6)  |
                               ((std::uint32_t)v3);

        out_bytes.push_back((char)((triple >> 16) & 0xFF));
        if (s[i + 2] != '=')
            out_bytes.push_back((char)((triple >> 8) & 0xFF));
        if (s[i + 3] != '=')
            out_bytes.push_back((char)((triple >> 0) & 0xFF));
    }

    // Trim padding-derived bytes if needed (defensive).
    if (pad && out_bytes.size() >= pad)
        out_bytes.resize(out_bytes.size() - pad);
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


