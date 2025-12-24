#include "core/encodings.h"

#include "core/encodings_tables_generated.h"

#include <unordered_map>

namespace phos::encodings
{
static const char32_t* Table(EncodingId enc)
{
    switch (enc)
    {
        case EncodingId::Cp437: return kCp437;
        case EncodingId::Cp850: return kCp850;
        case EncodingId::Cp852: return kCp852;
        case EncodingId::Cp855: return kCp855;
        case EncodingId::Cp857: return kCp857;
        case EncodingId::Cp860: return kCp860;
        case EncodingId::Cp861: return kCp861;
        case EncodingId::Cp862: return kCp862;
        case EncodingId::Cp863: return kCp863;
        case EncodingId::Cp865: return kCp865;
        case EncodingId::Cp866: return kCp866;
        case EncodingId::Cp775: return kCp775;
        case EncodingId::Cp737: return kCp737;
        case EncodingId::Cp869: return kCp869;
        case EncodingId::AmigaLatin1: return kAmigaLatin1;
        case EncodingId::AmigaIso8859_15: return kAmigaIso8859_15;
        case EncodingId::AmigaIso8859_2: return kAmigaIso8859_2;
        case EncodingId::Amiga1251: return kAmiga1251;
        default: return kCp437;
    }
}

char32_t ByteToUnicode(EncodingId enc, std::uint8_t b)
{
    return Table(enc)[b];
}

static const std::unordered_map<char32_t, std::uint8_t>& ReverseMap(EncodingId enc)
{
    // Build a separate reverse map per encoding (lazy init).
    // Note: mappings are not guaranteed to be bijective; we keep the first byte encountered.
    struct Cache
    {
        bool built = false;
        std::unordered_map<char32_t, std::uint8_t> map;
    };
    static Cache caches[256] = {};

    Cache& c = caches[(std::uint8_t)enc];
    if (!c.built)
    {
        c.map.clear();
        c.map.reserve(256);
        const char32_t* t = Table(enc);
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            const char32_t cp = t[i];
            if (c.map.find(cp) == c.map.end())
                c.map.emplace(cp, (std::uint8_t)i);
        }
        c.built = true;
    }
    return c.map;
}

bool UnicodeToByte(EncodingId enc, char32_t cp, std::uint8_t& out_b)
{
    const auto& map = ReverseMap(enc);
    auto it = map.find(cp);
    if (it == map.end())
        return false;
    out_b = it->second;
    return true;
}
} // namespace phos::encodings


