#include "io/project_file.h"

#include "core/canvas.h"
#include "io/session/project_state_json.h"

#include <nlohmann/json.hpp>
#include <zstd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace project_file
{
namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr unsigned char kPhosZstdMagic[4] = {'U', '8', 'P', 'Z'};
static constexpr std::uint32_t kPhosZstdVersion = 1;

static void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back((std::uint8_t)((v >> 0) & 0xFF));
    out.push_back((std::uint8_t)((v >> 8) & 0xFF));
    out.push_back((std::uint8_t)((v >> 16) & 0xFF));
    out.push_back((std::uint8_t)((v >> 24) & 0xFF));
}

static void AppendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((std::uint8_t)((v >> (8 * i)) & 0xFF));
}

static bool ReadU32LE(const std::vector<std::uint8_t>& in, size_t off, std::uint32_t& out)
{
    if (off + 4 > in.size())
        return false;
    out = (std::uint32_t)in[off + 0] | ((std::uint32_t)in[off + 1] << 8) |
          ((std::uint32_t)in[off + 2] << 16) | ((std::uint32_t)in[off + 3] << 24);
    return true;
}

static bool ReadU64LE(const std::vector<std::uint8_t>& in, size_t off, std::uint64_t& out)
{
    if (off + 8 > in.size())
        return false;
    out = 0;
    for (int i = 0; i < 8; ++i)
        out |= ((std::uint64_t)in[off + (size_t)i]) << (8 * i);
    return true;
}

static bool HasPhosZstdHeader(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 4 && bytes[0] == kPhosZstdMagic[0] && bytes[1] == kPhosZstdMagic[1] &&
           bytes[2] == kPhosZstdMagic[2] && bytes[3] == kPhosZstdMagic[3];
}

static bool ZstdCompress(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out, std::string& err)
{
    err.clear();
    out.clear();

    const size_t bound = ZSTD_compressBound(in.size());
    out.resize(bound);

    const int level = 3; // fast default; tweak later if needed
    const size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n))
    {
        err = std::string("zstd compress failed: ") + ZSTD_getErrorName(n);
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}

static bool ZstdDecompressKnownSize(const std::vector<std::uint8_t>& in,
                                    std::uint64_t uncompressed_size,
                                    std::vector<std::uint8_t>& out,
                                    std::string& err)
{
    err.clear();
    out.clear();

    if (uncompressed_size > (std::uint64_t)std::numeric_limits<size_t>::max())
    {
        err = "zstd decompress failed: uncompressed size too large for this platform.";
        return false;
    }
    out.resize((size_t)uncompressed_size);

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

static std::vector<std::uint8_t> ReadAllBytes(const std::string& path, std::string& err)
{
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file for reading.";
        return {};
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0)
    {
        err = "Failed to read file size.";
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(sz));
    if (sz > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return {};
    }
    return bytes;
}

static bool WriteAllBytesAtomic(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string& err)
{
    err.clear();
    try
    {
        fs::path p(path);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());

        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                err = "Failed to open file for writing.";
                return false;
            }
            if (!bytes.empty())
                out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            out.close();
            if (!out)
            {
                err = "Failed to write file contents.";
                return false;
            }
        }

        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec)
        {
            err = std::string("Failed to replace file: ") + ec.message();
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }
}
} // namespace

bool SaveProjectToFile(const std::string& path, const AnsiCanvas& canvas, std::string& err)
{
    err.clear();
    const auto st = canvas.GetProjectState();
    const json j = project_state_json::ToJson(st);

    std::vector<std::uint8_t> bytes;
    try
    {
        bytes = json::to_cbor(j);
    }
    catch (const std::exception& e)
    {
        err = std::string("CBOR encode failed: ") + e.what();
        return false;
    }

    // Compress CBOR payload with zstd.
    std::vector<std::uint8_t> compressed;
    std::string zerr;
    if (!ZstdCompress(bytes, compressed, zerr))
    {
        err = zerr;
        return false;
    }

    // File format:
    //   4 bytes  magic: "U8PZ"
    //   4 bytes  version (LE): 1
    //   8 bytes  uncompressed size (LE): CBOR byte length
    //   ...      zstd-compressed CBOR
    std::vector<std::uint8_t> out;
    out.reserve(4 + 4 + 8 + compressed.size());
    out.insert(out.end(), std::begin(kPhosZstdMagic), std::end(kPhosZstdMagic));
    AppendU32LE(out, kPhosZstdVersion);
    AppendU64LE(out, (std::uint64_t)bytes.size());
    out.insert(out.end(), compressed.begin(), compressed.end());

    return WriteAllBytesAtomic(path, out, err);
}

bool LoadProjectFromFile(const std::string& path, AnsiCanvas& out_canvas, std::string& err)
{
    err.clear();
    std::string read_err;
    const auto bytes = ReadAllBytes(path, read_err);
    if (!read_err.empty())
    {
        err = read_err;
        return false;
    }

    json j;
    // New format: zstd-wrapped CBOR with U8PZ header.
    if (HasPhosZstdHeader(bytes))
    {
        std::uint32_t ver = 0;
        std::uint64_t ulen = 0;
        if (!ReadU32LE(bytes, 4, ver) || !ReadU64LE(bytes, 8, ulen))
        {
            err = "Invalid project header.";
            return false;
        }
        if (ver != kPhosZstdVersion)
        {
            err = "Unsupported project version.";
            return false;
        }
        if (bytes.size() < 16)
        {
            err = "Invalid project header (truncated).";
            return false;
        }
        const std::vector<std::uint8_t> comp(bytes.begin() + 16, bytes.end());
        std::vector<std::uint8_t> cbor;
        std::string zerr;
        if (!ZstdDecompressKnownSize(comp, ulen, cbor, zerr))
        {
            err = zerr;
            return false;
        }
        try
        {
            j = json::from_cbor(cbor);
        }
        catch (const std::exception& e)
        {
            err = std::string("CBOR decode failed: ") + e.what();
            return false;
        }
    }
    else
    {
        // Backward compatibility: older uncompressed CBOR files.
        try
        {
            j = json::from_cbor(bytes);
        }
        catch (const std::exception& e)
        {
            err = std::string("CBOR decode failed: ") + e.what();
            return false;
        }
    }

    AnsiCanvas::ProjectState st;
    if (!project_state_json::FromJson(j, st, err))
        return false;

    std::string apply_err;
    if (!out_canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply project state." : apply_err;
        return false;
    }
    return true;
}
} // namespace project_file


