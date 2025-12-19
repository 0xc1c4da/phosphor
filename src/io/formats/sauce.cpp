#include "io/formats/sauce.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <unordered_map>

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

static constexpr std::array<char32_t, 256> kCp437ToUnicode = []() constexpr {
    // Standard IBM CP437 mapping.
    std::array<char32_t, 256> t{};
    t[0] = U'\u0000'; t[1] = U'\u263A'; t[2] = U'\u263B'; t[3] = U'\u2665'; t[4] = U'\u2666'; t[5] = U'\u2663'; t[6] = U'\u2660'; t[7] = U'\u2022';
    t[8] = U'\u25D8'; t[9] = U'\u25CB'; t[10] = U'\u25D9'; t[11] = U'\u2642'; t[12] = U'\u2640'; t[13] = U'\u266A'; t[14] = U'\u266B'; t[15] = U'\u263C';
    t[16] = U'\u25BA'; t[17] = U'\u25C4'; t[18] = U'\u2195'; t[19] = U'\u203C'; t[20] = U'\u00B6'; t[21] = U'\u00A7'; t[22] = U'\u25AC'; t[23] = U'\u21A8';
    t[24] = U'\u2191'; t[25] = U'\u2193'; t[26] = U'\u2192'; t[27] = U'\u2190'; t[28] = U'\u221F'; t[29] = U'\u2194'; t[30] = U'\u25B2'; t[31] = U'\u25BC';
    t[32] = U' '; t[33] = U'!'; t[34] = U'"'; t[35] = U'#'; t[36] = U'$'; t[37] = U'%'; t[38] = U'&'; t[39] = U'\'';
    t[40] = U'('; t[41] = U')'; t[42] = U'*'; t[43] = U'+'; t[44] = U','; t[45] = U'-'; t[46] = U'.'; t[47] = U'/';
    t[48] = U'0'; t[49] = U'1'; t[50] = U'2'; t[51] = U'3'; t[52] = U'4'; t[53] = U'5'; t[54] = U'6'; t[55] = U'7';
    t[56] = U'8'; t[57] = U'9'; t[58] = U':'; t[59] = U';'; t[60] = U'<'; t[61] = U'='; t[62] = U'>'; t[63] = U'?';
    t[64] = U'@'; t[65] = U'A'; t[66] = U'B'; t[67] = U'C'; t[68] = U'D'; t[69] = U'E'; t[70] = U'F'; t[71] = U'G';
    t[72] = U'H'; t[73] = U'I'; t[74] = U'J'; t[75] = U'K'; t[76] = U'L'; t[77] = U'M'; t[78] = U'N'; t[79] = U'O';
    t[80] = U'P'; t[81] = U'Q'; t[82] = U'R'; t[83] = U'S'; t[84] = U'T'; t[85] = U'U'; t[86] = U'V'; t[87] = U'W';
    t[88] = U'X'; t[89] = U'Y'; t[90] = U'Z'; t[91] = U'['; t[92] = U'\\'; t[93] = U']'; t[94] = U'^'; t[95] = U'_';
    t[96] = U'`'; t[97] = U'a'; t[98] = U'b'; t[99] = U'c'; t[100] = U'd'; t[101] = U'e'; t[102] = U'f'; t[103] = U'g';
    t[104] = U'h'; t[105] = U'i'; t[106] = U'j'; t[107] = U'k'; t[108] = U'l'; t[109] = U'm'; t[110] = U'n'; t[111] = U'o';
    t[112] = U'p'; t[113] = U'q'; t[114] = U'r'; t[115] = U's'; t[116] = U't'; t[117] = U'u'; t[118] = U'v'; t[119] = U'w';
    t[120] = U'x'; t[121] = U'y'; t[122] = U'z'; t[123] = U'{'; t[124] = U'|'; t[125] = U'}'; t[126] = U'~'; t[127] = U'\u2302';
    t[128] = U'\u00C7'; t[129] = U'\u00FC'; t[130] = U'\u00E9'; t[131] = U'\u00E2'; t[132] = U'\u00E4'; t[133] = U'\u00E0'; t[134] = U'\u00E5'; t[135] = U'\u00E7';
    t[136] = U'\u00EA'; t[137] = U'\u00EB'; t[138] = U'\u00E8'; t[139] = U'\u00EF'; t[140] = U'\u00EE'; t[141] = U'\u00EC'; t[142] = U'\u00C4'; t[143] = U'\u00C5';
    t[144] = U'\u00C9'; t[145] = U'\u00E6'; t[146] = U'\u00C6'; t[147] = U'\u00F4'; t[148] = U'\u00F6'; t[149] = U'\u00F2'; t[150] = U'\u00FB'; t[151] = U'\u00F9';
    t[152] = U'\u00FF'; t[153] = U'\u00D6'; t[154] = U'\u00DC'; t[155] = U'\u00A2'; t[156] = U'\u00A3'; t[157] = U'\u00A5'; t[158] = U'\u20A7'; t[159] = U'\u0192';
    t[160] = U'\u00E1'; t[161] = U'\u00ED'; t[162] = U'\u00F3'; t[163] = U'\u00FA'; t[164] = U'\u00F1'; t[165] = U'\u00D1'; t[166] = U'\u00AA'; t[167] = U'\u00BA';
    t[168] = U'\u00BF'; t[169] = U'\u2310'; t[170] = U'\u00AC'; t[171] = U'\u00BD'; t[172] = U'\u00BC'; t[173] = U'\u00A1'; t[174] = U'\u00AB'; t[175] = U'\u00BB';
    t[176] = U'\u2591'; t[177] = U'\u2592'; t[178] = U'\u2593'; t[179] = U'\u2502'; t[180] = U'\u2524'; t[181] = U'\u2561'; t[182] = U'\u2562'; t[183] = U'\u2556';
    t[184] = U'\u2555'; t[185] = U'\u2563'; t[186] = U'\u2551'; t[187] = U'\u2557'; t[188] = U'\u255D'; t[189] = U'\u255C'; t[190] = U'\u255B'; t[191] = U'\u2510';
    t[192] = U'\u2514'; t[193] = U'\u2534'; t[194] = U'\u252C'; t[195] = U'\u251C'; t[196] = U'\u2500'; t[197] = U'\u253C'; t[198] = U'\u255E'; t[199] = U'\u255F';
    t[200] = U'\u255A'; t[201] = U'\u2554'; t[202] = U'\u2569'; t[203] = U'\u2566'; t[204] = U'\u2560'; t[205] = U'\u2550'; t[206] = U'\u256C'; t[207] = U'\u2567';
    t[208] = U'\u2568'; t[209] = U'\u2564'; t[210] = U'\u2565'; t[211] = U'\u2559'; t[212] = U'\u2558'; t[213] = U'\u2552'; t[214] = U'\u2553'; t[215] = U'\u256B';
    t[216] = U'\u256A'; t[217] = U'\u2518'; t[218] = U'\u250C'; t[219] = U'\u2588'; t[220] = U'\u2584'; t[221] = U'\u258C'; t[222] = U'\u2590'; t[223] = U'\u2580';
    t[224] = U'\u03B1'; t[225] = U'\u00DF'; t[226] = U'\u0393'; t[227] = U'\u03C0'; t[228] = U'\u03A3'; t[229] = U'\u03C3'; t[230] = U'\u00B5'; t[231] = U'\u03C4';
    t[232] = U'\u03A6'; t[233] = U'\u0398'; t[234] = U'\u03A9'; t[235] = U'\u03B4'; t[236] = U'\u221E'; t[237] = U'\u03C6'; t[238] = U'\u03B5'; t[239] = U'\u2229';
    t[240] = U'\u2261'; t[241] = U'\u00B1'; t[242] = U'\u2265'; t[243] = U'\u2264'; t[244] = U'\u2320'; t[245] = U'\u2321'; t[246] = U'\u00F7'; t[247] = U'\u2248';
    t[248] = U'\u00B0'; t[249] = U'\u2219'; t[250] = U'\u00B7'; t[251] = U'\u221A'; t[252] = U'\u207F'; t[253] = U'\u00B2'; t[254] = U'\u25A0'; t[255] = U'\u00A0';
    return t;
}();

static std::unordered_map<char32_t, std::uint8_t> BuildUnicodeToCp437()
{
    std::unordered_map<char32_t, std::uint8_t> m;
    m.reserve(256);
    for (size_t i = 0; i < 256; ++i)
        m.emplace(kCp437ToUnicode[i], (std::uint8_t)i);
    return m;
}

static std::uint8_t UnicodeToCp437Byte(char32_t cp)
{
    static const std::unordered_map<char32_t, std::uint8_t> map = BuildUnicodeToCp437();
    auto it = map.find(cp);
    return (it == map.end()) ? (std::uint8_t)'?' : it->second;
}

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
            Utf8Append(kCp437ToUnicode[b], out);
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

static void WriteU16LE(std::uint8_t* p, std::uint16_t v)
{
    p[0] = (std::uint8_t)(v & 0xFF);
    p[1] = (std::uint8_t)((v >> 8) & 0xFF);
}

static void WriteU32LE(std::uint8_t* p, std::uint32_t v)
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
            out[o++] = UnicodeToCp437Byte(cp);
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


