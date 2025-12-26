#include "io/formats/gpl.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace formats::gpl
{
const std::vector<std::string_view>& ImportExtensions()
{
    static const std::vector<std::string_view> exts = {"gpl"};
    return exts;
}

namespace
{
static bool IsSpace(unsigned char c)
{
    return std::isspace(c) != 0;
}

static std::string_view TrimAscii(std::string_view s)
{
    size_t b = 0;
    while (b < s.size() && IsSpace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && IsSpace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

static bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

static void SkipSpaces(const char*& p, const char* end)
{
    while (p < end && IsSpace((unsigned char)*p))
        ++p;
}

static bool ParseU8Dec(const char*& p, const char* end, int& out_val)
{
    SkipSpaces(p, end);
    if (p >= end)
        return false;

    // Reject signs.
    if (*p == '-' || *p == '+')
        return false;

    int v = 0;
    int n = 0;
    while (p < end && *p >= '0' && *p <= '9')
    {
        v = v * 10 + (*p - '0');
        ++p;
        ++n;
        if (v > 255)
        {
            // Keep scanning digits so we consume the token, but mark invalid.
            while (p < end && *p >= '0' && *p <= '9')
                ++p;
            return false;
        }
    }
    if (n == 0)
        return false;

    out_val = v;
    return true;
}

static bool ReadAllBytesLimited(const std::string& path,
                                std::vector<std::uint8_t>& out,
                                std::string& err,
                                std::size_t limit_bytes)
{
    err.clear();
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file for reading.";
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0)
    {
        err = "Failed to read file size.";
        return false;
    }
    if ((std::uint64_t)sz > (std::uint64_t)limit_bytes)
    {
        err = "File too large.";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (sz > 0)
        in.read(reinterpret_cast<char*>(out.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return false;
    }
    return true;
}

} // namespace

bool ImportBytesToPalette(const std::vector<std::uint8_t>& bytes,
                          Palette& out_palette,
                          std::string& err,
                          std::string_view fallback_name)
{
    err.clear();
    out_palette = Palette{};
    out_palette.name = std::string(fallback_name);

    if (bytes.empty())
    {
        err = "Empty file.";
        return false;
    }

    // Treat bytes as text (GPL is ASCII; we accept UTF-8 in names/comments).
    const char* data = reinterpret_cast<const char*>(bytes.data());
    const size_t len = bytes.size();

    auto next_line = [&](size_t& i) -> std::string_view
    {
        if (i >= len)
            return {};
        size_t start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r')
            ++i;
        size_t end = i;
        // Normalize CRLF/CR: skip \r\n or \r
        if (i < len && data[i] == '\r')
        {
            ++i;
            if (i < len && data[i] == '\n')
                ++i;
        }
        else if (i < len && data[i] == '\n')
        {
            ++i;
        }
        return std::string_view(data + start, end - start);
    };

    size_t i = 0;

    // Read first non-empty line (allow UTF-8 BOM).
    std::string_view first;
    while (i < len)
    {
        first = TrimAscii(next_line(i));
        if (!first.empty())
            break;
    }
    if (first.size() >= 3 &&
        (unsigned char)first[0] == 0xEF &&
        (unsigned char)first[1] == 0xBB &&
        (unsigned char)first[2] == 0xBF)
    {
        first.remove_prefix(3);
        first = TrimAscii(first);
    }

    if (first != "GIMP Palette")
    {
        err = "Missing magic header (expected 'GIMP Palette').";
        return false;
    }

    while (i < len)
    {
        std::string_view line = next_line(i);
        line = TrimAscii(line);
        if (line.empty())
            continue;

        if (line[0] == '#')
            continue;

        if (StartsWith(line, "Name:"))
        {
            std::string_view v = TrimAscii(line.substr(5));
            if (!v.empty())
                out_palette.name.assign(v.begin(), v.end());
            continue;
        }

        if (StartsWith(line, "Columns:"))
        {
            std::string_view v = TrimAscii(line.substr(8));
            int cols = 0;
            // parse int (0..256), be forgiving
            const char* p = v.data();
            const char* e = v.data() + v.size();
            SkipSpaces(p, e);
            bool neg = false;
            if (p < e && *p == '-')
            {
                neg = true;
                ++p;
            }
            int n = 0;
            while (p < e && *p >= '0' && *p <= '9')
            {
                cols = cols * 10 + (*p - '0');
                ++p;
                ++n;
                if (cols > 256)
                    break;
            }
            if (!neg && n > 0 && cols >= 0 && cols <= 256)
                out_palette.columns = cols;
            continue;
        }

        // Colour line: "R G B [optional name...]"
        {
            const char* p = line.data();
            const char* e = line.data() + line.size();
            int r = 0, g = 0, b = 0;
            if (!ParseU8Dec(p, e, r) || !ParseU8Dec(p, e, g) || !ParseU8Dec(p, e, b))
                continue;

            // Rest is name (may include spaces).
            SkipSpaces(p, e);
            std::string_view nm(p, (size_t)(e - p));
            nm = TrimAscii(nm);

            Colour c;
            c.r = (std::uint8_t)std::clamp(r, 0, 255);
            c.g = (std::uint8_t)std::clamp(g, 0, 255);
            c.b = (std::uint8_t)std::clamp(b, 0, 255);
            if (!nm.empty())
                c.name.assign(nm.begin(), nm.end());
            out_palette.colours.push_back(std::move(c));
        }
    }

    if (out_palette.colours.empty())
    {
        err = "No colours found in palette.";
        return false;
    }

    if (out_palette.name.empty())
        out_palette.name = "GIMP Palette";

    return true;
}

bool ImportFileToPalette(const std::string& path,
                         Palette& out_palette,
                         std::string& err,
                         std::string_view fallback_name)
{
    std::vector<std::uint8_t> bytes;
    // Palettes are small; cap at 2 MiB to match other text importers.
    if (!ReadAllBytesLimited(path, bytes, err, (std::size_t)(2u * 1024u * 1024u)))
        return false;
    return ImportBytesToPalette(bytes, out_palette, err, fallback_name);
}

} // namespace formats::gpl


