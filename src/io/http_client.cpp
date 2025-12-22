#include "io/http_client.h"

#include "core/paths.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>

namespace http
{
namespace
{
static size_t WriteToVector(void* contents, size_t size, size_t nmemb, void* userp)
{
    const size_t n = size * nmemb;
    auto* out = reinterpret_cast<std::vector<std::uint8_t>*>(userp);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(contents);
    out->insert(out->end(), p, p + n);
    return n;
}

static void EnsureCurlGlobalInit()
{
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// Stable, tiny hash for cache keys (collision risk is negligible for our usage).
static std::uint64_t Fnv1a64(std::string_view s)
{
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s)
    {
        h ^= (std::uint64_t)c;
        h *= 1099511628211ull;
    }
    return h;
}

static std::string Hex64(std::uint64_t x)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << (unsigned long long)x;
    return oss.str();
}

static bool IsCacheableGet(const std::map<std::string, std::string>& headers)
{
    // Avoid caching requests that look user/session-specific.
    for (const auto& kv : headers)
    {
        const std::string k = ToLowerAscii(kv.first);
        if (k == "authorization" || k == "cookie" || k == "proxy-authorization")
            return false;
    }
    return true;
}

static std::filesystem::path HttpCacheFileFor(const std::string& url,
                                              const std::map<std::string, std::string>& headers)
{
    std::string key;
    key.reserve(url.size() + 128);
    key += url;
    key.push_back('\n');
    for (const auto& kv : headers)
    {
        key += ToLowerAscii(kv.first);
        key.push_back(':');
        key += kv.second;
        key.push_back('\n');
    }

    const std::string name = Hex64(Fnv1a64(key)) + ".bin";
    return std::filesystem::path(GetPhosphorCacheDir()) / "http" / name;
}

static bool CacheFileLooksPresent(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec)
        return false;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec)
        return false;
    return sz > 0;
}

static bool ReadFileBytes(const std::filesystem::path& p,
                          std::vector<std::uint8_t>& out,
                          std::string& err,
                          std::uintmax_t max_bytes = 100u * 1024u * 1024u)
{
    err.clear();
    out.clear();

    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec)
        return false;
    if (sz == 0 || sz > max_bytes)
        return false;

    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;

    out.resize((size_t)sz);
    if (!f.read((char*)out.data(), (std::streamsize)out.size()))
    {
        err = "Failed to read cached file bytes.";
        out.clear();
        return false;
    }
    return true;
}

static bool ReadFileBytesNoErr(const std::filesystem::path& p,
                               std::vector<std::uint8_t>& out,
                               std::uintmax_t max_bytes = 100u * 1024u * 1024u)
{
    std::string err;
    return ReadFileBytes(p, out, err, max_bytes);
}

static bool BytesEqual(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
{
    if (a.size() != b.size())
        return false;
    if (a.empty())
        return true;
    return std::equal(a.begin(), a.end(), b.begin());
}

static bool WriteFileBytesAtomic(const std::filesystem::path& p,
                                 const std::vector<std::uint8_t>& bytes,
                                 std::string& err)
{
    err.clear();
    try
    {
        std::filesystem::create_directories(p.parent_path());
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }

    const std::filesystem::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err = "Failed to open cache temp file for writing.";
            return false;
        }
        if (!bytes.empty())
            out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        out.close();
        if (!out)
        {
            err = "Failed to finalize cache temp file write.";
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec)
    {
        err = ec.message();
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }

    return true;
}
} // namespace

Response Get(const std::string& url, const std::map<std::string, std::string>& headers, CacheMode cache_mode)
{
    Response r;
    r.status = 0;
    r.body.clear();
    r.err.clear();
    r.from_cache = false;
    r.changed = false;

    // Disk cache (default: ~/.config/phosphor/cache/http/...)
    const bool cacheable = IsCacheableGet(headers);
    const std::filesystem::path cache_file = cacheable ? HttpCacheFileFor(url, headers) : std::filesystem::path();
    if (cacheable && cache_mode != CacheMode::NetworkOnly)
    {
        std::string cerr;
        if (ReadFileBytes(cache_file, r.body, cerr))
        {
            r.status = 200;
            r.err.clear();
            r.from_cache = true;
            return r;
        }
        // If the cache looks corrupt, best-effort remove it so we can refresh cleanly.
        if (!cerr.empty())
        {
            std::error_code ec;
            std::filesystem::remove(cache_file, ec);
        }
    }

    if (cache_mode == CacheMode::CacheOnly)
    {
        r.status = 0;
        r.err = "cache miss";
        return r;
    }

    EnsureCurlGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        r.err = "curl_easy_init failed.";
        return r;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip/deflate/br when built with support
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "phosphor/0.0 (https://github.com/)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToVector);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // Build header list.
    curl_slist* hdrs = nullptr;
    for (const auto& kv : headers)
    {
        std::string line = kv.first + ": " + kv.second;
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    if (hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK)
    {
        r.err = curl_easy_strerror(code);
        if (hdrs)
            curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return r;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r.status = status;

    if (hdrs)
        curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (r.status < 200 || r.status >= 300)
    {
        std::ostringstream oss;
        oss << "HTTP " << r.status;
        r.err = oss.str();
    }
    else if (cacheable)
    {
        // Persist successful responses. Content from 16colo.rs (packs, thumbnails, raw artwork)
        // is effectively static, so we keep it indefinitely to improve UX and reduce API load.
        //
        // To support "stale-while-revalidate" UX, only update the on-disk cache if the
        // network response is actually different from the existing cached bytes.
        std::vector<std::uint8_t> prev;
        const bool had_prev = ReadFileBytesNoErr(cache_file, prev);
        const bool same = had_prev ? BytesEqual(prev, r.body) : false;
        r.changed = !had_prev || !same;
        if (r.changed)
        {
            std::string werr;
            (void)WriteFileBytesAtomic(cache_file, r.body, werr);
        }
    }

    return r;
}

bool HasCached(const std::string& url, const std::map<std::string, std::string>& headers)
{
    const bool cacheable = IsCacheableGet(headers);
    if (!cacheable)
        return false;
    const std::filesystem::path cache_file = HttpCacheFileFor(url, headers);
    return CacheFileLooksPresent(cache_file);
}
} // namespace http


