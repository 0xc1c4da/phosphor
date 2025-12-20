#include "core/embedded_assets.h"

#include "core/paths.h"

#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern "C" {
// Produced by: ld -r -b binary build/phosphor_assets.tar.zst
extern const unsigned char _binary_build_phosphor_assets_tar_zst_start[];
extern const unsigned char _binary_build_phosphor_assets_tar_zst_end[];
}

static bool IsAllZero(const unsigned char* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0)
            return false;
    return true;
}

static uint64_t ParseOctal(const char* s, size_t n)
{
    // tar stores numbers as NUL/space padded octal.
    uint64_t out = 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0'))
        ++i;
    for (; i < n; ++i)
    {
        const char c = s[i];
        if (c == ' ' || c == '\0')
            break;
        if (c < '0' || c > '7')
            break;
        out = (out << 3) + (uint64_t)(c - '0');
    }
    return out;
}

static bool IsSafeRelativePath(const fs::path& p)
{
    if (p.empty() || p.is_absolute())
        return false;
    for (const auto& part : p)
    {
        const std::string s = part.string();
        if (s == "." || s.empty())
            continue;
        if (s == "..")
            return false;
    }
    return true;
}

static bool ExtractTarNoClobber(const std::vector<unsigned char>& tar,
                                const fs::path& dest_root,
                                std::string& error)
{
    error.clear();
    if (tar.size() < 512)
    {
        error = "Embedded tar is too small";
        return false;
    }

    try
    {
        fs::create_directories(dest_root);
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    size_t off = 0;
    while (off + 512 <= tar.size())
    {
        const unsigned char* blk = tar.data() + off;
        if (IsAllZero(blk, 512))
            break;

        // USTAR header (only fields we need).
        const char* name = (const char*)(blk + 0);       // 100
        const char* size = (const char*)(blk + 124);     // 12
        const char  type = (char)blk[156];               // 1
        const char* prefix = (const char*)(blk + 345);   // 155

        auto read_field = [](const char* p, size_t n) -> std::string {
            size_t len = 0;
            while (len < n && p[len] != '\0')
                ++len;
            return std::string(p, p + len);
        };

        std::string path = read_field(name, 100);
        const std::string pre = read_field(prefix, 155);
        if (!pre.empty())
            path = pre + "/" + path;

        // Normalize: strip any leading "./"
        while (path.size() >= 2 && path[0] == '.' && path[1] == '/')
            path.erase(0, 2);

        // Normalize: strip any leading "/" (treat as relative) and trailing slashes.
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        while (!path.empty() && path.back() == '/')
            path.pop_back();

        const uint64_t file_size = ParseOctal(size, 12);
        off += 512;

        // Some tar producers include explicit "." / "./" directory entries; ignore them.
        if (path.empty() || path == ".")
        {
            const size_t padded = (size_t)(((file_size + 511ull) / 512ull) * 512ull);
            off += padded;
            continue;
        }

        // Ignore PAX extended headers (we don't need them for our simple asset archive).
        // They are type 'x' (per-file) or 'g' (global).
        if (type == 'x' || type == 'g')
        {
            const size_t padded = (size_t)(((file_size + 511ull) / 512ull) * 512ull);
            off += padded;
            continue;
        }

        const fs::path rel(path);
        if (!IsSafeRelativePath(rel))
        {
            error = "Unsafe path in embedded tar: " + path;
            return false;
        }

        const fs::path out_path = (dest_root / rel).lexically_normal();

        // Guard against escaping dest_root via normalization tricks.
        const fs::path norm_root = dest_root.lexically_normal();
        const auto out_str = out_path.string();
        const auto root_str = norm_root.string();
        if (out_str.size() < root_str.size() || out_str.compare(0, root_str.size(), root_str) != 0)
        {
            error = "Path escapes destination root: " + out_str;
            return false;
        }

        if (type == '5')
        {
            // Directory
            try { fs::create_directories(out_path); }
            catch (const std::exception& e) { error = e.what(); return false; }
        }
        else
        {
            // Regular file ('0' or '\0' common)
            if (off + (size_t)file_size > tar.size())
            {
                error = "Truncated embedded tar payload";
                return false;
            }

            if (!fs::exists(out_path))
            {
                try { fs::create_directories(out_path.parent_path()); }
                catch (const std::exception& e) { error = e.what(); return false; }

                std::ofstream f(out_path, std::ios::binary);
                if (!f)
                {
                    error = "Failed to write " + out_path.string();
                    return false;
                }
                f.write((const char*)tar.data() + off, (std::streamsize)file_size);
                f.close();
            }
        }

        // Advance to next header: payload padded to 512 bytes.
        const size_t padded = (size_t)(((file_size + 511ull) / 512ull) * 512ull);
        off += padded;
    }

    return true;
}

static bool ZstdDecompressToVector(const unsigned char* src, size_t src_size,
                                  std::vector<unsigned char>& out,
                                  std::string& error)
{
    error.clear();
    out.clear();

    const unsigned long long expected = ZSTD_getFrameContentSize(src, src_size);
    if (expected != ZSTD_CONTENTSIZE_ERROR && expected != ZSTD_CONTENTSIZE_UNKNOWN)
    {
        out.resize((size_t)expected);
        const size_t r = ZSTD_decompress(out.data(), out.size(), src, src_size);
        if (ZSTD_isError(r))
        {
            error = ZSTD_getErrorName(r);
            out.clear();
            return false;
        }
        out.resize(r);
        return true;
    }

    // Unknown size: streaming decompress.
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds)
    {
        error = "ZSTD_createDStream failed";
        return false;
    }
    size_t r = ZSTD_initDStream(ds);
    if (ZSTD_isError(r))
    {
        error = ZSTD_getErrorName(r);
        ZSTD_freeDStream(ds);
        return false;
    }

    ZSTD_inBuffer in{src, src_size, 0};
    std::vector<unsigned char> buf;
    buf.resize(1u << 20); // 1 MiB chunks

    while (in.pos < in.size)
    {
        ZSTD_outBuffer o{buf.data(), buf.size(), 0};
        r = ZSTD_decompressStream(ds, &o, &in);
        if (ZSTD_isError(r))
        {
            error = ZSTD_getErrorName(r);
            ZSTD_freeDStream(ds);
            out.clear();
            return false;
        }
        out.insert(out.end(), buf.data(), buf.data() + o.pos);
    }

    ZSTD_freeDStream(ds);
    return true;
}

bool EnsureBundledAssetsExtracted(std::string& error)
{
    error.clear();

    const fs::path dest_root = fs::path(GetPhosphorAssetsDir());
    const fs::path marker = dest_root / ".phosphor-assets-extracted";

    auto dir_has_any_with_ext = [&](const fs::path& dir, const char* ext) -> bool {
        try
        {
            if (!fs::exists(dir) || !fs::is_directory(dir))
                return false;
            for (const auto& it : fs::directory_iterator(dir))
            {
                if (!it.is_regular_file())
                    continue;
                if (it.path().extension() == ext)
                    return true;
            }
        }
        catch (...)
        {
            return false;
        }
        return false;
    };

    // If we previously extracted, we're done unless some key files are missing.
    if (fs::exists(marker) &&
        fs::exists(dest_root / "character-palettes.json") &&
        fs::exists(dest_root / "color-palettes.json") &&
        fs::exists(dest_root / "key-bindings.json") &&
        fs::exists(dest_root / "character-sets.json") &&
        fs::exists(dest_root / "session.json") &&
        // Ensure font assets exist too (older installs may have the marker but no fonts).
        dir_has_any_with_ext(dest_root / "fonts" / "flf", ".flf") &&
        dir_has_any_with_ext(dest_root / "fonts" / "tdf", ".tdf"))
    {
        return true;
    }

    const unsigned char* start = _binary_build_phosphor_assets_tar_zst_start;
    const unsigned char* end = _binary_build_phosphor_assets_tar_zst_end;
    const size_t src_size = (size_t)(end - start);
    if (src_size == 0)
    {
        error = "Embedded assets blob is empty";
        return false;
    }

    std::vector<unsigned char> tar;
    if (!ZstdDecompressToVector(start, src_size, tar, error))
        return false;

    if (!ExtractTarNoClobber(tar, dest_root, error))
        return false;

    // Mark successful extraction (so we don't redo it every run).
    try
    {
        fs::create_directories(dest_root);
        std::ofstream m(marker, std::ios::binary);
        if (m)
            m << "ok\n";
    }
    catch (...)
    {
        // Non-fatal: assets may still be present; extraction succeeded.
    }

    return true;
}


