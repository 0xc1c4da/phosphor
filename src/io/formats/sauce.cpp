#include "io/formats/sauce.h"

#include "core/encodings.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>

namespace sauce
{
namespace
{
static constexpr std::uint8_t kSub = 0x1A;

// SAUCE field offsets within the 128-byte record (SAUCE 00).
// See references/sauce-spec.md.
static constexpr size_t OFF_ID        = 0;   // 5
static constexpr size_t OFF_VERSION   = 5;   // 2
static constexpr size_t OFF_TITLE     = 7;   // 35
static constexpr size_t OFF_AUTHOR    = 42;  // 20
static constexpr size_t OFF_GROUP     = 62;  // 20
static constexpr size_t OFF_DATE      = 82;  // 8
static constexpr size_t OFF_FILESIZE  = 90;  // 4 (u32 LE)
static constexpr size_t OFF_DATATYPE  = 94;  // 1
static constexpr size_t OFF_FILETYPE  = 95;  // 1
static constexpr size_t OFF_TINFO1    = 96;  // 2 (u16 LE)
static constexpr size_t OFF_TINFO2    = 98;  // 2
static constexpr size_t OFF_TINFO3    = 100; // 2
static constexpr size_t OFF_TINFO4    = 102; // 2
static constexpr size_t OFF_COMMENTS  = 104; // 1
static constexpr size_t OFF_TFLAGS    = 105; // 1
static constexpr size_t OFF_TINFOS    = 106; // 22 (ZString)

static void Utf8Append(char32_t cp, std::string& out)
{
    char buf[5] = {0, 0, 0, 0, 0};
    if (cp <= 0x7F)
    {
        buf[0] = (char)cp;
        out.append(buf, buf + 1);
        return;
    }
    if (cp <= 0x7FF)
    {
        buf[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        out.append(buf, buf + 2);
        return;
    }
    if (cp <= 0xFFFF)
    {
        buf[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        out.append(buf, buf + 3);
        return;
    }
    buf[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    out.append(buf, buf + 4);
}

static std::string DecodeFixedCharField(const std::uint8_t* p, size_t n, bool decode_cp437)
{
    // Trim trailing spaces and NULs.
    while (n > 0 && (p[n - 1] == 0 || p[n - 1] == 32))
        --n;
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const std::uint8_t b = p[i];
        if (decode_cp437)
            Utf8Append(phos::encodings::ByteToUnicode(phos::encodings::EncodingId::Cp437, b), out);
        else
            out.push_back((char)b);
    }
    return out;
}

static std::uint16_t ReadU16LE(const std::uint8_t* p)
{
    return (std::uint16_t)p[0] | ((std::uint16_t)p[1] << 8);
}

static std::uint32_t ReadU32LE(const std::uint8_t* p)
{
    return (std::uint32_t)p[0] |
           ((std::uint32_t)p[1] << 8) |
           ((std::uint32_t)p[2] << 16) |
           ((std::uint32_t)p[3] << 24);
}

[[maybe_unused]] static void WriteU16LE(std::uint8_t* p, std::uint16_t v)
{
    p[0] = (std::uint8_t)(v & 0xFF);
    p[1] = (std::uint8_t)((v >> 8) & 0xFF);
}

[[maybe_unused]] static void WriteU32LE(std::uint8_t* p, std::uint32_t v)
{
    p[0] = (std::uint8_t)((v >> 0) & 0xFF);
    p[1] = (std::uint8_t)((v >> 8) & 0xFF);
    p[2] = (std::uint8_t)((v >> 16) & 0xFF);
    p[3] = (std::uint8_t)((v >> 24) & 0xFF);
}

static bool IsSauce00(const std::uint8_t* rec)
{
    return rec[0] == 'S' && rec[1] == 'A' && rec[2] == 'U' && rec[3] == 'C' && rec[4] == 'E' &&
           rec[5] == '0' && rec[6] == '0';
}

static std::vector<std::string> DecodeCommentLines(const std::uint8_t* p, size_t count, bool decode_cp437)
{
    std::vector<std::string> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        out.push_back(DecodeFixedCharField(p + i * 64, 64, decode_cp437));
    }
    return out;
}

static void ChunkAndAppendComments(const std::vector<std::string>& in, std::vector<std::string>& out)
{
    out.clear();
    for (const std::string& s : in)
    {
        if (s.empty())
        {
            out.push_back(std::string());
            continue;
        }
        // Split by Unicode codepoints (not bytes) so we don't cut UTF-8 sequences.
        size_t i = 0;
        while (i < s.size())
        {
            size_t start = i;
            size_t cps = 0;
            while (i < s.size() && cps < 64)
            {
                unsigned char c = (unsigned char)s[i];
                size_t len = 1;
                if (c < 0x80) len = 1;
                else if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                else len = 1;
                if (i + len > s.size())
                    break;
                bool ok = true;
                for (size_t k = 1; k < len; ++k)
                {
                    unsigned char cc = (unsigned char)s[i + k];
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                }
                if (!ok) len = 1;
                i += len;
                ++cps;
            }
            out.push_back(s.substr(start, i - start));
        }
    }
    if (out.size() > 255)
        out.resize(255);
}

} // namespace

void FilterControlChars(std::string& s)
{
    // SAUCE "Character" fields are plain text; drop control chars (incl. newlines/tabs).
    // We keep bytes >= 0x20 as-is; UTF-8 multi-byte sequences are preserved.
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c >= 0x20 && c != 0x7F)
            out.push_back((char)c);
    }
    s.swap(out);
}

void KeepOnlyDigits(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c >= '0' && c <= '9')
            out.push_back((char)c);
    }
    s.swap(out);
}

size_t Utf8CodepointCount(std::string_view s)
{
    size_t i = 0;
    size_t cps = 0;
    while (i < s.size())
    {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > s.size())
            break;
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) len = 1;
        i += len;
        ++cps;
    }
    return cps;
}

void TrimUtf8ToCodepoints(std::string& s, size_t max_codepoints)
{
    if (max_codepoints == 0)
    {
        s.clear();
        return;
    }

    size_t i = 0;
    size_t cps = 0;
    const size_t n = s.size();
    while (i < n && cps < max_codepoints)
    {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            len = 1; // invalid leading byte; treat as single byte

        if (i + len > n)
            break;

        // Validate continuation bytes; if invalid, consume one byte.
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
        }
        if (!ok)
            len = 1;

        i += len;
        ++cps;
    }

    if (i < s.size())
        s.resize(i);
}

bool ParseDateYYYYMMDD(std::string_view s, int& y, int& m, int& d)
{
    if (s.size() != 8)
        return false;
    for (unsigned char c : s)
        if (c < '0' || c > '9')
            return false;

    auto to_i = [](std::string_view sv) -> int { return std::atoi(std::string(sv).c_str()); };
    y = to_i(s.substr(0, 4));
    m = to_i(s.substr(4, 2));
    d = to_i(s.substr(6, 2));
    if (!(y >= 1900 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31))
        return false;
    auto is_leap = [](int yy) { return (yy % 400 == 0) || ((yy % 4 == 0) && (yy % 100 != 0)); };
    auto days_in_month = [&](int mm, int yy) -> int {
        static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int days = mdays[mm - 1];
        if (mm == 2 && is_leap(yy))
            days = 29;
        return days;
    };
    return d <= days_in_month(m, y);
}

void FormatDateYYYYMMDD(int y, int m, int d, std::string& out)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", y, m, d);
    out = buf;
}

std::string TodayYYYYMMDD()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::string out;
    FormatDateYYYYMMDD(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, out);
    return out;
}

namespace
{
static void SanitizeRecordForWrite(Record& r)
{
    // Fixed-width "Character" fields: strip control characters.
    FilterControlChars(r.title);
    FilterControlChars(r.author);
    FilterControlChars(r.group);
    FilterControlChars(r.tinfos);
    for (std::string& line : r.comments)
        FilterControlChars(line);

    // Date: must be exactly 8 digits CCYYMMDD, otherwise treat as empty.
    KeepOnlyDigits(r.date);
    int y = 0, m = 0, d = 0;
    if (!ParseDateYYYYMMDD(r.date, y, m, d))
        r.date.clear();
}
} // namespace

std::vector<std::uint8_t> EncodeCharField(std::string_view s, size_t width, bool encode_cp437)
{
    std::vector<std::uint8_t> out(width, (std::uint8_t)' ');
    // Very small UTF-8 decoder (only to map to CP437). We accept invalid input and replace with '?'.
    size_t i = 0;
    size_t o = 0;
    while (i < s.size() && o < width)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
        {
            out[o++] = (std::uint8_t)c;
            i++;
            continue;
        }

        // Decode a UTF-8 sequence into a codepoint (best-effort).
        char32_t cp = U'?';
        size_t remaining = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; remaining = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; remaining = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; remaining = 3; }
        else { i++; out[o++] = (std::uint8_t)'?'; continue; }

        if (i + remaining >= s.size())
            break;
        bool ok = true;
        for (size_t j = 0; j < remaining; ++j)
        {
            unsigned char cc = (unsigned char)s[i + 1 + j];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += 1 + remaining;
        if (!ok)
        {
            out[o++] = (std::uint8_t)'?';
            continue;
        }

        if (encode_cp437)
        {
            std::uint8_t b = (std::uint8_t)'?';
            (void)phos::encodings::UnicodeToByte(phos::encodings::EncodingId::Cp437, cp, b);
            out[o++] = b;
        }
        else
            out[o++] = (cp <= 0x7F) ? (std::uint8_t)cp : (std::uint8_t)'?';
    }
    return out;
}

bool ParseFromBytes(const std::vector<std::uint8_t>& bytes, Parsed& out, std::string& err, bool decode_cp437)
{
    err.clear();
    out = Parsed{};
    out.payload_size = bytes.size();

    if (bytes.size() < kSauceRecordSize)
        return true;

    const size_t sauce_off = bytes.size() - kSauceRecordSize;
    const std::uint8_t* rec = bytes.data() + sauce_off;
    if (!IsSauce00(rec))
        return true; // no SAUCE

    Record r;
    r.present = true;
    r.title  = DecodeFixedCharField(rec + OFF_TITLE, 35, decode_cp437);
    r.author = DecodeFixedCharField(rec + OFF_AUTHOR, 20, decode_cp437);
    r.group  = DecodeFixedCharField(rec + OFF_GROUP, 20, decode_cp437);
    r.date   = DecodeFixedCharField(rec + OFF_DATE, 8, false /* date is always ASCII digits */);
    r.file_size = ReadU32LE(rec + OFF_FILESIZE);
    r.data_type = rec[OFF_DATATYPE];
    r.file_type = rec[OFF_FILETYPE];
    r.tinfo1 = ReadU16LE(rec + OFF_TINFO1);
    r.tinfo2 = ReadU16LE(rec + OFF_TINFO2);
    r.tinfo3 = ReadU16LE(rec + OFF_TINFO3);
    r.tinfo4 = ReadU16LE(rec + OFF_TINFO4);
    r.comments_count = rec[OFF_COMMENTS];
    r.tflags = rec[OFF_TFLAGS];

    // TInfoS is a zero-terminated string (ZString) within 22 bytes.
    {
        const std::uint8_t* p = rec + OFF_TINFOS;
        size_t n = 0;
        while (n < 22 && p[n] != 0)
            n++;
        r.tinfos = DecodeFixedCharField(p, n, decode_cp437);
    }

    size_t payload_end = sauce_off;
    bool has_comment_block = false;

    // Optional comment block.
    if (r.comments_count > 0)
    {
        const size_t need = kSauceCommentHeaderSize + (size_t)r.comments_count * 64;
        if (payload_end >= need)
        {
            const size_t comnt_off = payload_end - need;
            const std::uint8_t* hdr = bytes.data() + comnt_off;
            if (hdr[0] == 'C' && hdr[1] == 'O' && hdr[2] == 'M' && hdr[3] == 'N' && hdr[4] == 'T')
            {
                has_comment_block = true;
                const std::uint8_t* lines = hdr + kSauceCommentHeaderSize;
                r.comments = DecodeCommentLines(lines, r.comments_count, decode_cp437);
                payload_end = comnt_off;
            }
        }
    }

    // Optional EOF (Ctrl+Z) right before metadata.
    bool has_eof = false;
    if (payload_end > 0 && bytes[payload_end - 1] == kSub)
    {
        has_eof = true;
        payload_end -= 1;
    }

    out.record = std::move(r);
    out.has_comment_block = has_comment_block;
    out.has_eof_byte = has_eof;
    out.payload_size = payload_end;
    return true;
}

size_t ComputePayloadSize(const std::vector<std::uint8_t>& bytes)
{
    Parsed p;
    std::string err;
    if (!ParseFromBytes(bytes, p, err, true))
        return bytes.size();
    return p.record.present ? p.payload_size : bytes.size();
}

std::vector<std::uint8_t> StripFromBytes(const std::vector<std::uint8_t>& bytes)
{
    const size_t n = ComputePayloadSize(bytes);
    return std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + n);
}

std::vector<std::uint8_t> AppendToBytes(const std::vector<std::uint8_t>& payload,
                                        const Record& record,
                                        const WriteOptions& opt,
                                        std::string& err)
{
    err.clear();
    if (!record.present)
        return payload;

    // Enforce SAUCE spec constraints at the encoder boundary.
    Record r = record;
    SanitizeRecordForWrite(r);

    // Prepare comment lines (already line-based in record, but also chunk any long lines).
    std::vector<std::string> comment_lines;
    if (opt.include_comments && !r.comments.empty())
        ChunkAndAppendComments(r.comments, comment_lines);

    if (comment_lines.size() > 255)
    {
        err = "Too many SAUCE comment lines (max 255).";
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + 1 + (kSauceCommentHeaderSize + comment_lines.size() * 64) + kSauceRecordSize);
    out.insert(out.end(), payload.begin(), payload.end());

    if (opt.include_eof_byte)
        out.push_back(kSub);

    if (!comment_lines.empty())
    {
        out.push_back((std::uint8_t)'C');
        out.push_back((std::uint8_t)'O');
        out.push_back((std::uint8_t)'M');
        out.push_back((std::uint8_t)'N');
        out.push_back((std::uint8_t)'T');
        for (const std::string& line : comment_lines)
        {
            const auto bytes64 = EncodeCharField(line, 64, opt.encode_cp437);
            out.insert(out.end(), bytes64.begin(), bytes64.end());
        }
    }

    std::array<std::uint8_t, kSauceRecordSize> rec{};
    std::memset(rec.data(), (int)' ', rec.size());

    // ID + Version
    rec[0] = 'S'; rec[1] = 'A'; rec[2] = 'U'; rec[3] = 'C'; rec[4] = 'E';
    rec[5] = '0'; rec[6] = '0';

    // Fixed-width fields
    {
        const auto t = EncodeCharField(r.title, 35, opt.encode_cp437);
        std::memcpy(rec.data() + OFF_TITLE, t.data(), t.size());
        const auto a = EncodeCharField(r.author, 20, opt.encode_cp437);
        std::memcpy(rec.data() + OFF_AUTHOR, a.data(), a.size());
        const auto g = EncodeCharField(r.group, 20, opt.encode_cp437);
        std::memcpy(rec.data() + OFF_GROUP, g.data(), g.size());
        const auto d = EncodeCharField(r.date, 8, false);
        std::memcpy(rec.data() + OFF_DATE, d.data(), d.size());
    }

    const std::uint32_t file_size = r.file_size ? r.file_size : (std::uint32_t)payload.size();
    WriteU32LE(rec.data() + OFF_FILESIZE, file_size);
    rec[OFF_DATATYPE] = r.data_type;
    rec[OFF_FILETYPE] = r.file_type;
    WriteU16LE(rec.data() + OFF_TINFO1, r.tinfo1);
    WriteU16LE(rec.data() + OFF_TINFO2, r.tinfo2);
    WriteU16LE(rec.data() + OFF_TINFO3, r.tinfo3);
    WriteU16LE(rec.data() + OFF_TINFO4, r.tinfo4);
    rec[OFF_COMMENTS] = (std::uint8_t)comment_lines.size();
    rec[OFF_TFLAGS] = r.tflags;

    // TInfoS: ZString within 22 bytes (NUL padded).
    {
        std::array<std::uint8_t, 22> tinfos{};
        tinfos.fill(0);
        // Encode up to 22 characters (bytes) and leave the rest NUL.
        const auto s = EncodeCharField(r.tinfos, 22, opt.encode_cp437);
        size_t n = s.size();
        while (n > 0 && s[n - 1] == (std::uint8_t)' ')
            --n;
        if (n > tinfos.size()) n = tinfos.size();
        std::memcpy(tinfos.data(), s.data(), n);
        std::memcpy(rec.data() + OFF_TINFOS, tinfos.data(), tinfos.size());
    }

    out.insert(out.end(), rec.begin(), rec.end());
    return out;
}
} // namespace sauce


