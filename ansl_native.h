#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>

// Forward-declare Dear ImGui font type so hosts can optionally use it without
// pulling imgui.h into every translation unit including this header.
struct ImFont;

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

struct Vec4
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
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

// Text helpers used by native hosts (LuaJIT, etc).
// These operate on UTF-8 input and count "width" in Unicode codepoints (not terminal column width).
namespace text
{
struct MeasureResult
{
    int numLines = 0;
    int maxWidth = 0;
};

inline MeasureResult measure_utf8(const char* s, size_t len)
{
    MeasureResult r;
    if (!s || len == 0)
        return r;

    std::vector<char32_t> cps;
    utf8::decode_to_codepoints(s, len, cps);
    if (cps.empty())
        return r;

    int lineWidth = 0;
    r.numLines = 1;
    for (char32_t cp : cps)
    {
        if (cp == U'\n')
        {
            r.maxWidth = std::max(r.maxWidth, lineWidth);
            lineWidth = 0;
            r.numLines++;
        }
        else
        {
            lineWidth++;
            r.maxWidth = std::max(r.maxWidth, lineWidth);
        }
    }
    return r;
}

struct WrapResult
{
    std::string text;
    int numLines = 0;
    int maxWidth = 0;
};

// Wraps at spaces without breaking "words". Multiple spaces are preserved as single spaces
// between wrapped words (mirroring the original JS behavior which splits on ' ').
inline WrapResult wrap_utf8(const char* s, size_t len, int width)
{
    WrapResult out;
    if (!s || len == 0)
        return out;

    if (width <= 0)
    {
        out.text.assign(s, s + len);
        const auto m = measure_utf8(s, len);
        out.numLines = m.numLines;
        out.maxWidth = m.maxWidth;
        return out;
    }

    std::vector<char32_t> cps;
    utf8::decode_to_codepoints(s, len, cps);

    auto flush_word = [&](std::u32string& word, std::u32string& line, std::u32string& acc) {
        if (word.empty())
            return;
        if (line.empty())
        {
            line = word;
        }
        else
        {
            // try add " " + word
            if ((int)line.size() + 1 + (int)word.size() <= width)
            {
                line.push_back(U' ');
                line.append(word);
            }
            else
            {
                acc.append(line);
                acc.push_back(U'\n');
                line = word;
            }
        }
        word.clear();
    };

    std::u32string acc;
    std::u32string line;
    std::u32string word;

    for (char32_t cp : cps)
    {
        if (cp == U'\n')
        {
            flush_word(word, line, acc);
            acc.append(line);
            acc.push_back(U'\n');
            line.clear();
            word.clear();
        }
        else if (cp == U' ')
        {
            flush_word(word, line, acc);
        }
        else
        {
            word.push_back(cp);
        }
    }
    flush_word(word, line, acc);
    acc.append(line);

    // Remove trailing newline if input didn't end with one.
    // The JS impl always appends '\n' after each paragraph and then slices the last one off.
    // Here we keep exactly what was accumulated; measure determines line counts.

    // Encode
    std::string encoded;
    encoded.reserve(len + 16);
    for (char32_t cp : acc)
        encoded += utf8::encode(cp);

    out.text = std::move(encoded);
    const auto m = measure_utf8(out.text.c_str(), out.text.size());
    out.numLines = m.numLines;
    out.maxWidth = m.maxWidth;
    return out;
}
} // namespace text

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

// GLSL-style mod: x - y * floor(x / y). This differs from fmod for negative x.
inline double mod_glsl(double x, double y)
{
    if (y == 0.0)
        return 0.0;
    return x - y * std::floor(x / y);
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
inline double lengthSq(const Vec3& a) { return a.x * a.x + a.y * a.y + a.z * a.z; }
inline Vec3 abs(const Vec3& a) { return {std::abs(a.x), std::abs(a.y), std::abs(a.z)}; }
inline Vec3 max(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 min(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 norm(const Vec3& a)
{
    const double l = length(a);
    if (l > 0.00001)
        return {a.x / l, a.y / l, a.z / l};
    return {0.0, 0.0, 0.0};
}
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

// ---- Boolean operators (hard) ----
inline double opUnion(double a, double b) { return std::min(a, b); }
inline double opIntersection(double a, double b) { return std::max(a, b); }
inline double opDifference(double a, double b) { return std::max(a, -b); }

// ---- HG_SDF construction kit (ported from references/hg_sdf.glsl) ----
namespace hg
{
// Constants: keep as doubles; PHI is numeric to avoid constexpr sqrt portability issues.
constexpr double PI = 3.14159265;
constexpr double TAU = 2.0 * PI;
constexpr double PHI = 1.6180339887498948482;

inline double saturate(double x) { return num::clamp(x, 0.0, 1.0); }
inline double sgn(double x) { return (x < 0.0) ? -1.0 : 1.0; } // never returns 0
inline Vec2 sgn(const Vec2& v) { return {(v.x < 0.0) ? -1.0 : 1.0, (v.y < 0.0) ? -1.0 : 1.0}; }

inline double square(double x) { return x * x; }
inline Vec2 square(const Vec2& x) { return {x.x * x.x, x.y * x.y}; }
inline Vec3 square(const Vec3& x) { return {x.x * x.x, x.y * x.y, x.z * x.z}; }
inline double lengthSqr(const Vec3& x) { return vec3::dot(x, x); }

inline double vmax(const Vec2& v) { return std::max(v.x, v.y); }
inline double vmax(const Vec3& v) { return std::max(std::max(v.x, v.y), v.z); }
inline double vmax(const Vec4& v) { return std::max(std::max(v.x, v.y), std::max(v.z, v.w)); }
inline double vmin(const Vec2& v) { return std::min(v.x, v.y); }
inline double vmin(const Vec3& v) { return std::min(std::min(v.x, v.y), v.z); }
inline double vmin(const Vec4& v) { return std::min(std::min(v.x, v.y), std::min(v.z, v.w)); }

inline Vec2 v2(double x, double y) { return {x, y}; }
inline Vec3 v3(double x, double y, double z) { return {x, y, z}; }

inline Vec2 v2_max(const Vec2& a, const Vec2& b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline Vec2 v2_min(const Vec2& a, const Vec2& b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }

inline Vec2 normalize(const Vec2& a) { return vec2::norm(a); }
inline Vec3 normalize(const Vec3& a) { return vec3::norm(a); }

// ---- Primitive distance functions ----
inline double fSphere(const Vec3& p, double r) { return vec3::length(p) - r; }
inline double fPlane(const Vec3& p, const Vec3& n, double distanceFromOrigin) { return vec3::dot(p, n) + distanceFromOrigin; }

inline double fBoxCheap(const Vec3& p, const Vec3& b) { return vmax(vec3::sub(vec3::abs(p), b)); }
inline double fBox(const Vec3& p, const Vec3& b)
{
    const Vec3 d = vec3::sub(vec3::abs(p), b);
    const Vec3 d0 = {std::max(d.x, 0.0), std::max(d.y, 0.0), std::max(d.z, 0.0)};
    const Vec3 d1 = {std::min(d.x, 0.0), std::min(d.y, 0.0), std::min(d.z, 0.0)};
    return vec3::length(d0) + vmax(d1);
}

inline double fBox2Cheap(const Vec2& p, const Vec2& b) { return vmax(vec2::sub(vec2::abs(p), b)); }
inline double fBox2(const Vec2& p, const Vec2& b)
{
    const Vec2 d = vec2::sub(vec2::abs(p), b);
    const Vec2 d0 = {std::max(d.x, 0.0), std::max(d.y, 0.0)};
    const Vec2 d1 = {std::min(d.x, 0.0), std::min(d.y, 0.0)};
    return vec2::length(d0) + vmax(d1);
}

inline double fCorner(const Vec2& p)
{
    return vec2::length(v2_max(p, {0.0, 0.0})) + vmax(v2_min(p, {0.0, 0.0}));
}

// Not a correct distance bound (ported as-is).
inline double fBlob(Vec3 p)
{
    p = vec3::abs(p);
    auto sw_yzx = [](const Vec3& a) -> Vec3 { return {a.y, a.z, a.x}; };
    if (p.x < std::max(p.y, p.z)) p = sw_yzx(p);
    if (p.x < std::max(p.y, p.z)) p = sw_yzx(p);

    const double b = std::max(
        std::max(
            std::max(vec3::dot(p, normalize(v3(1, 1, 1))),
                     vec2::dot(v2(p.x, p.z), normalize(v2(PHI + 1, 1)))),
            vec2::dot(v2(p.y, p.x), normalize(v2(1, PHI)))),
        vec2::dot(v2(p.x, p.z), normalize(v2(1, PHI))));

    const double l = vec3::length(p);
    const double inner = std::sqrt(std::max(1.01 - b / l, 0.0));
    return l - 1.5 - 0.2 * (1.5 / 2.0) * std::cos(std::min(inner * (PI / 0.25), PI));
}

inline double fCylinder(const Vec3& p, double r, double height)
{
    double d = vec2::length(v2(p.x, p.z)) - r;
    d = std::max(d, std::abs(p.y) - height);
    return d;
}

inline double fCapsule(const Vec3& p, double r, double c)
{
    const double a = vec2::length(v2(p.x, p.z)) - r;
    const double b = vec3::length(v3(p.x, std::abs(p.y) - c, p.z)) - r;
    return num::mix(a, b, num::step(c, std::abs(p.y)));
}

inline double fLineSegment(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = vec3::sub(b, a);
    const double t = saturate(vec3::dot(vec3::sub(p, a), ab) / vec3::dot(ab, ab));
    return vec3::length(vec3::sub(vec3::add(vec3::mulN(ab, t), a), p));
}

inline double fCapsule(const Vec3& p, const Vec3& a, const Vec3& b, double r)
{
    return fLineSegment(p, a, b) - r;
}

inline double fTorus(const Vec3& p, double smallRadius, double largeRadius)
{
    return vec2::length(v2(vec2::length(v2(p.x, p.z)) - largeRadius, p.y)) - smallRadius;
}

inline double fCircle(const Vec3& p, double r)
{
    const double l = vec2::length(v2(p.x, p.z)) - r;
    return vec2::length(v2(p.y, l));
}

inline double fDisc(const Vec3& p, double r)
{
    const double l = vec2::length(v2(p.x, p.z)) - r;
    return (l < 0.0) ? std::abs(p.y) : vec2::length(v2(p.y, l));
}

inline double fHexagonCircumcircle(const Vec3& p, const Vec2& h)
{
    const Vec3 q = vec3::abs(p);
    return std::max(q.y - h.y, std::max(q.x * std::sqrt(3.0) * 0.5 + q.z * 0.5, q.z) - h.x);
}

inline double fHexagonIncircle(const Vec3& p, const Vec2& h)
{
    return fHexagonCircumcircle(p, v2(h.x * std::sqrt(3.0) * 0.5, h.y));
}

inline double fCone(const Vec3& p, double radius, double height)
{
    const Vec2 q = v2(vec2::length(v2(p.x, p.z)), p.y);
    const Vec2 tip = vec2::sub(q, v2(0.0, height));
    const Vec2 mantleDir = normalize(v2(height, radius));
    const double mantle = vec2::dot(tip, mantleDir);
    double d = std::max(mantle, -q.y);
    const double projected = vec2::dot(tip, v2(mantleDir.y, -mantleDir.x));

    if ((q.y > height) && (projected < 0.0))
        d = std::max(d, vec2::length(tip));

    if ((q.x > radius) && (projected > vec2::length(v2(height, radius))))
        d = std::max(d, vec2::length(vec2::sub(q, v2(radius, 0.0))));

    return d;
}

// ---- GDF primitives ----
inline const std::array<Vec3, 19>& GDFVectors()
{
    static const std::array<Vec3, 19> v = []() {
        auto N = [](const Vec3& a) { return normalize(a); };
        return std::array<Vec3, 19>{
            N(v3(1, 0, 0)),
            N(v3(0, 1, 0)),
            N(v3(0, 0, 1)),
            N(v3(1, 1, 1)),
            N(v3(-1, 1, 1)),
            N(v3(1, -1, 1)),
            N(v3(1, 1, -1)),
            N(v3(0, 1, PHI + 1)),
            N(v3(0, -1, PHI + 1)),
            N(v3(PHI + 1, 0, 1)),
            N(v3(-PHI - 1, 0, 1)),
            N(v3(1, PHI + 1, 0)),
            N(v3(-1, PHI + 1, 0)),
            N(v3(0, PHI, 1)),
            N(v3(0, -PHI, 1)),
            N(v3(1, 0, PHI)),
            N(v3(-1, 0, PHI)),
            N(v3(PHI, 1, 0)),
            N(v3(-PHI, 1, 0)),
        };
    }();
    return v;
}

inline double fGDF(const Vec3& p, double r, double e, int begin, int end)
{
    double d = 0.0;
    const auto& vecs = GDFVectors();
    for (int i = begin; i <= end; ++i)
        d += std::pow(std::abs(vec3::dot(p, vecs[(size_t)i])), e);
    return std::pow(d, 1.0 / e) - r;
}

inline double fGDF(const Vec3& p, double r, int begin, int end)
{
    double d = 0.0;
    const auto& vecs = GDFVectors();
    for (int i = begin; i <= end; ++i)
        d = std::max(d, std::abs(vec3::dot(p, vecs[(size_t)i])));
    return d - r;
}

inline double fOctahedron(const Vec3& p, double r, double e) { return fGDF(p, r, e, 3, 6); }
inline double fDodecahedron(const Vec3& p, double r, double e) { return fGDF(p, r, e, 13, 18); }
inline double fIcosahedron(const Vec3& p, double r, double e) { return fGDF(p, r, e, 3, 12); }
inline double fTruncatedOctahedron(const Vec3& p, double r, double e) { return fGDF(p, r, e, 0, 6); }
inline double fTruncatedIcosahedron(const Vec3& p, double r, double e) { return fGDF(p, r, e, 3, 18); }
inline double fOctahedron(const Vec3& p, double r) { return fGDF(p, r, 3, 6); }
inline double fDodecahedron(const Vec3& p, double r) { return fGDF(p, r, 13, 18); }
inline double fIcosahedron(const Vec3& p, double r) { return fGDF(p, r, 3, 12); }
inline double fTruncatedOctahedron(const Vec3& p, double r) { return fGDF(p, r, 0, 6); }
inline double fTruncatedIcosahedron(const Vec3& p, double r) { return fGDF(p, r, 3, 18); }

// ---- Domain manipulation operators (Lua-friendly return types) ----
struct Mod1Result { double p = 0.0; double c = 0.0; };
struct Mod2Result { Vec2 p{}; Vec2 c{}; };
struct Mod3Result { Vec3 p{}; Vec3 c{}; };
struct Mirror1Result { double p = 0.0; double s = 1.0; };
struct Mirror2Result { Vec2 p{}; Vec2 s{}; };
struct ReflectResult { Vec3 p{}; double s = 1.0; };

inline Vec2 pR(Vec2 p, double a)
{
    const double cs = std::cos(a);
    const double sn = std::sin(a);
    return {cs * p.x + sn * p.y, cs * p.y - sn * p.x};
}

inline Vec2 pR45(Vec2 p)
{
    const double k = std::sqrt(0.5);
    return {(p.x + p.y) * k, (p.y - p.x) * k};
}

inline Mod1Result pMod1(double p, double size)
{
    const double halfsize = size * 0.5;
    const double c = std::floor((p + halfsize) / size);
    const double pp = num::mod_glsl(p + halfsize, size) - halfsize;
    return {pp, c};
}

inline Mod1Result pModMirror1(double p, double size)
{
    const double halfsize = size * 0.5;
    const double c = std::floor((p + halfsize) / size);
    double pp = num::mod_glsl(p + halfsize, size) - halfsize;
    pp *= num::mod_glsl(c, 2.0) * 2.0 - 1.0;
    return {pp, c};
}

inline Mod1Result pModSingle1(double p, double size)
{
    const double halfsize = size * 0.5;
    const double c = std::floor((p + halfsize) / size);
    double pp = p;
    if (p >= 0.0)
        pp = num::mod_glsl(p + halfsize, size) - halfsize;
    return {pp, c};
}

inline Mod1Result pModInterval1(double p, double size, double start, double stop)
{
    const double halfsize = size * 0.5;
    double c = std::floor((p + halfsize) / size);
    double pp = num::mod_glsl(p + halfsize, size) - halfsize;
    if (c > stop)
    {
        pp += size * (c - stop);
        c = stop;
    }
    if (c < start)
    {
        pp += size * (c - start);
        c = start;
    }
    return {pp, c};
}

inline std::pair<Vec2, double> pModPolar(Vec2 p, double repetitions)
{
    const double angle = 2.0 * PI / repetitions;
    double a = std::atan2(p.y, p.x) + angle / 2.0;
    const double r = vec2::length(p);
    double c = std::floor(a / angle);
    a = num::mod_glsl(a, angle) - angle / 2.0;
    p = {std::cos(a) * r, std::sin(a) * r};
    if (std::abs(c) >= (repetitions / 2.0))
        c = std::abs(c);
    return {p, c};
}

inline Mod2Result pMod2(Vec2 p, const Vec2& size)
{
    const Vec2 c = {std::floor((p.x + size.x * 0.5) / size.x), std::floor((p.y + size.y * 0.5) / size.y)};
    p.x = num::mod_glsl(p.x + size.x * 0.5, size.x) - size.x * 0.5;
    p.y = num::mod_glsl(p.y + size.y * 0.5, size.y) - size.y * 0.5;
    return {p, c};
}

inline Mod2Result pModMirror2(Vec2 p, const Vec2& size)
{
    const Vec2 halfsize = {size.x * 0.5, size.y * 0.5};
    const Vec2 c = {std::floor((p.x + halfsize.x) / size.x), std::floor((p.y + halfsize.y) / size.y)};
    p.x = num::mod_glsl(p.x + halfsize.x, size.x) - halfsize.x;
    p.y = num::mod_glsl(p.y + halfsize.y, size.y) - halfsize.y;
    p.x *= num::mod_glsl(c.x, 2.0) * 2.0 - 1.0;
    p.y *= num::mod_glsl(c.y, 2.0) * 2.0 - 1.0;
    return {p, c};
}

inline Mod2Result pModGrid2(Vec2 p, const Vec2& size)
{
    Vec2 c = {std::floor((p.x + size.x * 0.5) / size.x), std::floor((p.y + size.y * 0.5) / size.y)};
    p.x = num::mod_glsl(p.x + size.x * 0.5, size.x) - size.x * 0.5;
    p.y = num::mod_glsl(p.y + size.y * 0.5, size.y) - size.y * 0.5;
    p.x *= num::mod_glsl(c.x, 2.0) * 2.0 - 1.0;
    p.y *= num::mod_glsl(c.y, 2.0) * 2.0 - 1.0;
    p = vec2::sub(p, vec2::divN(size, 2.0));
    if (p.x > p.y)
        std::swap(p.x, p.y);
    c = {std::floor(c.x / 2.0), std::floor(c.y / 2.0)};
    return {p, c};
}

inline Mod3Result pMod3(Vec3 p, const Vec3& size)
{
    const Vec3 c = {std::floor((p.x + size.x * 0.5) / size.x),
                    std::floor((p.y + size.y * 0.5) / size.y),
                    std::floor((p.z + size.z * 0.5) / size.z)};
    p.x = num::mod_glsl(p.x + size.x * 0.5, size.x) - size.x * 0.5;
    p.y = num::mod_glsl(p.y + size.y * 0.5, size.y) - size.y * 0.5;
    p.z = num::mod_glsl(p.z + size.z * 0.5, size.z) - size.z * 0.5;
    return {p, c};
}

inline Mirror1Result pMirror(double p, double dist)
{
    const double s = sgn(p);
    const double pp = std::abs(p) - dist;
    return {pp, s};
}

inline Mirror2Result pMirrorOctant(Vec2 p, const Vec2& dist)
{
    const Vec2 s = sgn(p);
    const auto mx = pMirror(p.x, dist.x);
    const auto my = pMirror(p.y, dist.y);
    p.x = mx.p;
    p.y = my.p;
    if (p.y > p.x)
        std::swap(p.x, p.y);
    return {p, s};
}

inline ReflectResult pReflect(Vec3 p, const Vec3& planeNormal, double offset)
{
    const double t = vec3::dot(p, planeNormal) + offset;
    if (t < 0.0)
        p = vec3::sub(p, vec3::mulN(planeNormal, 2.0 * t));
    return {p, sgn(t)};
}

// ---- Object combination operators ----
inline double fOpUnionChamfer(double a, double b, double r)
{
    return std::min(std::min(a, b), (a - r + b) * std::sqrt(0.5));
}

inline double fOpIntersectionChamfer(double a, double b, double r)
{
    return std::max(std::max(a, b), (a + r + b) * std::sqrt(0.5));
}

inline double fOpDifferenceChamfer(double a, double b, double r)
{
    return fOpIntersectionChamfer(a, -b, r);
}

inline double fOpUnionRound(double a, double b, double r)
{
    const Vec2 u = v2_max(v2(r - a, r - b), v2(0.0, 0.0));
    return std::max(r, std::min(a, b)) - vec2::length(u);
}

inline double fOpIntersectionRound(double a, double b, double r)
{
    const Vec2 u = v2_max(v2(r + a, r + b), v2(0.0, 0.0));
    return std::min(-r, std::max(a, b)) + vec2::length(u);
}

inline double fOpDifferenceRound(double a, double b, double r)
{
    return fOpIntersectionRound(a, -b, r);
}

inline double fOpUnionColumns(double a, double b, double r, double n)
{
    if ((a < r) && (b < r))
    {
        Vec2 p = v2(a, b);
        const double columnradius = r * std::sqrt(2.0) / ((n - 1.0) * 2.0 + std::sqrt(2.0));
        p = pR45(p);
        p.x -= std::sqrt(2.0) / 2.0 * r;
        p.x += columnradius * std::sqrt(2.0);
        if (num::mod_glsl(n, 2.0) == 1.0)
            p.y += columnradius;

        const auto m = pMod1(p.y, columnradius * 2.0);
        p.y = m.p;

        double result = vec2::length(p) - columnradius;
        result = std::min(result, p.x);
        result = std::min(result, a);
        return std::min(result, b);
    }
    return std::min(a, b);
}

inline double fOpDifferenceColumns(double a, double b, double r, double n)
{
    a = -a;
    const double m = std::min(a, b);
    if ((a < r) && (b < r))
    {
        Vec2 p = v2(a, b);
        const double columnradius = r * std::sqrt(2.0) / ((n - 1.0) * 2.0 + std::sqrt(2.0));
        p = pR45(p);
        p.y += columnradius;
        p.x -= std::sqrt(2.0) / 2.0 * r;
        p.x += -columnradius * std::sqrt(2.0) / 2.0;
        if (num::mod_glsl(n, 2.0) == 1.0)
            p.y += columnradius;

        const auto mm = pMod1(p.y, columnradius * 2.0);
        p.y = mm.p;

        double result = -vec2::length(p) + columnradius;
        result = std::max(result, p.x);
        result = std::min(result, a);
        return -std::min(result, b);
    }
    return -m;
}

inline double fOpIntersectionColumns(double a, double b, double r, double n)
{
    return fOpDifferenceColumns(a, -b, r, n);
}

inline double fOpUnionStairs(double a, double b, double r, double n)
{
    const double s = r / n;
    const double u = b - r;
    const double m = num::mod_glsl(u - a + s, 2.0 * s);
    return std::min(std::min(a, b), 0.5 * (u + a + std::abs(m - s)));
}

inline double fOpIntersectionStairs(double a, double b, double r, double n)
{
    return -fOpUnionStairs(-a, -b, r, n);
}

inline double fOpDifferenceStairs(double a, double b, double r, double n)
{
    return -fOpUnionStairs(-a, b, r, n);
}

inline double fOpUnionSoft(double a, double b, double r)
{
    const double e = std::max(r - std::abs(a - b), 0.0);
    return std::min(a, b) - e * e * 0.25 / r;
}

inline double fOpPipe(double a, double b, double r)
{
    return vec2::length(v2(a, b)) - r;
}

inline double fOpEngrave(double a, double b, double r)
{
    return std::max(a, (a + r - std::abs(b)) * std::sqrt(0.5));
}

inline double fOpGroove(double a, double b, double ra, double rb)
{
    return std::max(a, std::min(a + ra, rb - std::abs(b)));
}

inline double fOpTongue(double a, double b, double ra, double rb)
{
    return std::min(a, std::max(a - ra, std::abs(b) - rb));
}
} // namespace hg
} // namespace sdf

// Host-side helpers that depend on how glyphs are rasterized.
// In the native editor, we use Dear ImGui's font atlas to estimate "brightness"
// (ink coverage) of each glyph, analogous to the JS module `ansl/src/modules/sort.js`.
namespace sort
{
// Sort a UTF-8 charset by glyph brightness (ink coverage).
//
// - `charset_utf8`: UTF-8 string containing the glyphs to sort.
// - `font`: Dear ImGui font used to measure glyph coverage. If null, the host may
//   substitute a default font (implementation-dependent).
// - `ascending`: if true, darkest/least-ink first; otherwise brightest/most-ink first.
//
// Note: this sorts by Unicode codepoints (not grapheme clusters).
std::string by_brightness_utf8(const char* charset_utf8,
                               size_t len,
                               const ::ImFont* font,
                               bool ascending = false);

inline std::string by_brightness_utf8(const std::string& s,
                                      const ::ImFont* font,
                                      bool ascending = false)
{
    return by_brightness_utf8(s.c_str(), s.size(), font, ascending);
}
} // namespace sort
} // namespace ansl


