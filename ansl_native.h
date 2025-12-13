#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>

namespace ansl
{
struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Minimal UTF-8 helpers for hosts (LuaJIT, etc). These are intentionally permissive:
// malformed sequences are skipped or replaced with U+0020 ' ' to keep rendering robust.
namespace utf8
{
inline char32_t decode_first(const char* s, size_t len)
{
    if (!s || len == 0)
        return U' ';

    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    unsigned char c = p[0];
    if ((c & 0x80) == 0)
        return static_cast<char32_t>(c);

    size_t remaining = 0;
    char32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; remaining = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; remaining = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; remaining = 3; }
    else return U' ';

    if (remaining >= len)
        return U' ';
    for (size_t i = 0; i < remaining; ++i)
    {
        unsigned char cc = p[1 + i];
        if ((cc & 0xC0) != 0x80)
            return U' ';
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

inline void decode_to_codepoints(const char* s, size_t len, std::vector<char32_t>& out)
{
    out.clear();
    if (!s || len == 0)
        return;
    const unsigned char* data = reinterpret_cast<const unsigned char*>(s);
    size_t i = 0;
    while (i < len)
    {
        unsigned char c = data[i];
        char32_t cp = 0;
        size_t remaining = 0;
        if ((c & 0x80) == 0)
        {
            cp = c;
            remaining = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            remaining = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            remaining = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
            remaining = 3;
        }
        else
        {
            ++i;
            continue;
        }

        if (i + remaining >= len)
            break;

        bool malformed = false;
        for (size_t j = 0; j < remaining; ++j)
        {
            unsigned char cc = data[i + 1 + j];
            if ((cc & 0xC0) != 0x80)
            {
                malformed = true;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (malformed)
        {
            ++i;
            continue;
        }

        i += 1 + remaining;
        out.push_back(cp);
    }
}

inline std::string encode(char32_t cp)
{
    char out[5] = {0, 0, 0, 0, 0};
    int n = 0;
    if (cp <= 0x7F) { out[0] = (char)cp; n = 1; }
    else if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    }
    else if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    else
    {
        out[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return std::string(out, out + n);
}
} // namespace utf8

namespace num
{
inline double map(double v, double inA, double inB, double outA, double outB)
{
    return outA + (outB - outA) * ((v - inA) / (inB - inA));
}

inline double fract(double v)
{
    return v - std::floor(v);
}

inline double clamp(double v, double mn, double mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

inline double sign(double n)
{
    if (n > 0.0) return 1.0;
    if (n < 0.0) return -1.0;
    return 0.0;
}

inline double mix(double v1, double v2, double a)
{
    return v1 * (1.0 - a) + v2 * a;
}

inline double step(double edge, double x)
{
    return (x < edge) ? 0.0 : 1.0;
}

inline double smoothstep(double edge0, double edge1, double t)
{
    const double x = clamp((t - edge0) / (edge1 - edge0), 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

inline double smootherstep(double edge0, double edge1, double t)
{
    const double x = clamp((t - edge0) / (edge1 - edge0), 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

inline double mod(double a, double b)
{
    return std::fmod(a, b);
}
} // namespace num

namespace vec2
{
inline Vec2 vec2(double x, double y) { return {x, y}; }
inline Vec2 copy(const Vec2& a) { return a; }
inline Vec2 add(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 sub(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 mul(const Vec2& a, const Vec2& b) { return {a.x * b.x, a.y * b.y}; }
inline Vec2 div(const Vec2& a, const Vec2& b) { return {a.x / b.x, a.y / b.y}; }
inline Vec2 addN(const Vec2& a, double k) { return {a.x + k, a.y + k}; }
inline Vec2 subN(const Vec2& a, double k) { return {a.x - k, a.y - k}; }
inline Vec2 mulN(const Vec2& a, double k) { return {a.x * k, a.y * k}; }
inline Vec2 divN(const Vec2& a, double k) { return {a.x / k, a.y / k}; }
inline double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline double length(const Vec2& a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline double lengthSq(const Vec2& a) { return a.x * a.x + a.y * a.y; }
inline double dist(const Vec2& a, const Vec2& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}
inline double distSq(const Vec2& a, const Vec2& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}
inline Vec2 norm(const Vec2& a)
{
    const double l = length(a);
    if (l > 0.00001)
        return {a.x / l, a.y / l};
    return {0.0, 0.0};
}
inline Vec2 neg(const Vec2& v) { return {-v.x, -v.y}; }
inline Vec2 rot(const Vec2& a, double ang)
{
    const double s = std::sin(ang);
    const double c = std::cos(ang);
    return {a.x * c - a.y * s, a.x * s + a.y * c};
}
inline Vec2 mix(const Vec2& a, const Vec2& b, double t)
{
    return {(1.0 - t) * a.x + t * b.x, (1.0 - t) * a.y + t * b.y};
}
inline Vec2 abs(const Vec2& a) { return {std::abs(a.x), std::abs(a.y)}; }
inline Vec2 max(const Vec2& a, const Vec2& b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline Vec2 min(const Vec2& a, const Vec2& b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline Vec2 fract(const Vec2& a) { return {a.x - std::floor(a.x), a.y - std::floor(a.y)}; }
inline Vec2 floor(const Vec2& a) { return {std::floor(a.x), std::floor(a.y)}; }
inline Vec2 ceil(const Vec2& a) { return {std::ceil(a.x), std::ceil(a.y)}; }
inline Vec2 round(const Vec2& a) { return {std::round(a.x), std::round(a.y)}; }
} // namespace vec2

namespace vec3
{
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 div(const Vec3& a, const Vec3& b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline Vec3 addN(const Vec3& a, double k) { return {a.x + k, a.y + k, a.z + k}; }
inline Vec3 subN(const Vec3& a, double k) { return {a.x - k, a.y - k, a.z - k}; }
inline Vec3 mulN(const Vec3& a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline Vec3 divN(const Vec3& a, double k) { return {a.x / k, a.y / k, a.z / k}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
} // namespace vec3

namespace sdf
{
inline double sdCircle(const Vec2& p, double radius)
{
    return vec2::length(p) - radius;
}

inline double sdBox(const Vec2& p, const Vec2& size)
{
    Vec2 d{std::abs(p.x) - size.x, std::abs(p.y) - size.y};
    d.x = std::max(d.x, 0.0);
    d.y = std::max(d.y, 0.0);
    return vec2::length(d) + std::min(std::max(d.x, d.y), 0.0);
}

inline double sdSegment(const Vec2& p, const Vec2& a, const Vec2& b, double thickness)
{
    const Vec2 pa = vec2::sub(p, a);
    const Vec2 ba = vec2::sub(b, a);
    const double h = num::clamp(vec2::dot(pa, ba) / vec2::dot(ba, ba), 0.0, 1.0);
    return vec2::length(vec2::sub(pa, vec2::mulN(ba, h))) - thickness;
}

inline double opSmoothUnion(double d1, double d2, double k)
{
    const double h = num::clamp(0.5 + 0.5 * (d2 - d1) / k, 0.0, 1.0);
    return num::mix(d2, d1, h) - k * h * (1.0 - h);
}

inline double opSmoothSubtraction(double d1, double d2, double k)
{
    const double h = num::clamp(0.5 - 0.5 * (d2 + d1) / k, 0.0, 1.0);
    return num::mix(d2, -d1, h) + k * h * (1.0 - h);
}

inline double opSmoothIntersection(double d1, double d2, double k)
{
    const double h = num::clamp(0.5 - 0.5 * (d2 - d1) / k, 0.0, 1.0);
    return num::mix(d2, d1, h) + k * h * (1.0 - h);
}
} // namespace sdf
} // namespace ansl


