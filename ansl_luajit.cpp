#include "ansl_native.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

#include <string>
#include <algorithm>
#include <cmath>

#include "xterm256_palette.h"

namespace
{
// LuaJIT (Lua 5.1) compatibility: lua_absindex exists only in Lua 5.2+.
static int LuaAbsIndex(lua_State* L, int idx)
{
    // Pseudo-indices (registry, upvalues, etc.) should be returned as-is.
    if (idx > 0 || idx <= LUA_REGISTRYINDEX)
        return idx;
    return lua_gettop(L) + idx + 1;
}

static ansl::Vec2 LuaCheckVec2(lua_State* L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "x");
    const double x = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "y");
    const double y = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return {x, y};
}

static ansl::Vec3 LuaCheckVec3(lua_State* L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "x");
    const double x = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "y");
    const double y = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "z");
    const double z = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return {x, y, z};
}

static void LuaPushVec2(lua_State* L, const ansl::Vec2& v)
{
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
}

static void LuaSetVec2OnTop(lua_State* L, const ansl::Vec2& v)
{
    // table is at -1
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
}

static void LuaReturnVec2(lua_State* L, const ansl::Vec2& v, int out_idx)
{
    if (out_idx > 0 && lua_gettop(L) >= out_idx && lua_istable(L, out_idx))
    {
        lua_pushvalue(L, out_idx);
        LuaSetVec2OnTop(L, v);
    }
    else
    {
        LuaPushVec2(L, v);
    }
}

static void LuaPushVec3(lua_State* L, const ansl::Vec3& v)
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
}

static void LuaSetVec3OnTop(lua_State* L, const ansl::Vec3& v)
{
    // table is at -1
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
}

static void LuaReturnVec3(lua_State* L, const ansl::Vec3& v, int out_idx)
{
    if (out_idx > 0 && lua_gettop(L) >= out_idx && lua_istable(L, out_idx))
    {
        lua_pushvalue(L, out_idx);
        LuaSetVec3OnTop(L, v);
    }
    else
    {
        LuaPushVec3(L, v);
    }
}

// -------- num --------
static int l_num_map(lua_State* L)
{
    const double v = luaL_checknumber(L, 1);
    const double inA = luaL_checknumber(L, 2);
    const double inB = luaL_checknumber(L, 3);
    const double outA = luaL_checknumber(L, 4);
    const double outB = luaL_checknumber(L, 5);
    lua_pushnumber(L, ansl::num::map(v, inA, inB, outA, outB));
    return 1;
}

static int l_num_fract(lua_State* L)
{
    lua_pushnumber(L, ansl::num::fract(luaL_checknumber(L, 1)));
    return 1;
}

static int l_num_clamp(lua_State* L)
{
    lua_pushnumber(L, ansl::num::clamp(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_num_sign(lua_State* L)
{
    lua_pushnumber(L, ansl::num::sign(luaL_checknumber(L, 1)));
    return 1;
}

static int l_num_mix(lua_State* L)
{
    lua_pushnumber(L, ansl::num::mix(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_num_step(lua_State* L)
{
    lua_pushnumber(L, ansl::num::step(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_num_smoothstep(lua_State* L)
{
    lua_pushnumber(L, ansl::num::smoothstep(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_num_smootherstep(lua_State* L)
{
    lua_pushnumber(L, ansl::num::smootherstep(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_num_mod(lua_State* L)
{
    lua_pushnumber(L, ansl::num::mod(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

// -------- vec2 (subset) --------
static int l_vec2_vec2(lua_State* L)
{
    LuaReturnVec2(L, {luaL_checknumber(L, 1), luaL_checknumber(L, 2)}, 3);
    return 1;
}

static int l_vec2_copy(lua_State* L)
{
    LuaReturnVec2(L, LuaCheckVec2(L, 1), 2);
    return 1;
}

static int l_vec2_add(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::add(a, b), 3);
    return 1;
}

static int l_vec2_sub(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::sub(a, b), 3);
    return 1;
}

static int l_vec2_mul(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::mul(a, b), 3);
    return 1;
}

static int l_vec2_div(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::div(a, b), 3);
    return 1;
}

static int l_vec2_addN(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const double k = luaL_checknumber(L, 2);
    LuaReturnVec2(L, ansl::vec2::addN(a, k), 3);
    return 1;
}

static int l_vec2_subN(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const double k = luaL_checknumber(L, 2);
    LuaReturnVec2(L, ansl::vec2::subN(a, k), 3);
    return 1;
}

static int l_vec2_mulN(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const double k = luaL_checknumber(L, 2);
    LuaReturnVec2(L, ansl::vec2::mulN(a, k), 3);
    return 1;
}

static int l_vec2_divN(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const double k = luaL_checknumber(L, 2);
    LuaReturnVec2(L, ansl::vec2::divN(a, k), 3);
    return 1;
}

static int l_vec2_dot(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    lua_pushnumber(L, ansl::vec2::dot(a, b));
    return 1;
}

static int l_vec2_length(lua_State* L)
{
    lua_pushnumber(L, ansl::vec2::length(LuaCheckVec2(L, 1)));
    return 1;
}

static int l_vec2_lengthSq(lua_State* L)
{
    lua_pushnumber(L, ansl::vec2::lengthSq(LuaCheckVec2(L, 1)));
    return 1;
}

static int l_vec2_dist(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    lua_pushnumber(L, ansl::vec2::dist(a, b));
    return 1;
}

static int l_vec2_distSq(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    lua_pushnumber(L, ansl::vec2::distSq(a, b));
    return 1;
}

static int l_vec2_norm(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::norm(a), 2);
    return 1;
}

static int l_vec2_neg(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::neg(a), 2);
    return 1;
}

static int l_vec2_rot(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const double ang = luaL_checknumber(L, 2);
    LuaReturnVec2(L, ansl::vec2::rot(a, ang), 3);
    return 1;
}

static int l_vec2_mix(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    const double t = luaL_checknumber(L, 3);
    LuaReturnVec2(L, ansl::vec2::mix(a, b, t), 4);
    return 1;
}

static int l_vec2_abs(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::abs(a), 2);
    return 1;
}

static int l_vec2_max(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::max(a, b), 3);
    return 1;
}

static int l_vec2_min(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    const auto b = LuaCheckVec2(L, 2);
    LuaReturnVec2(L, ansl::vec2::min(a, b), 3);
    return 1;
}

static int l_vec2_fract(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::fract(a), 2);
    return 1;
}

static int l_vec2_floor(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::floor(a), 2);
    return 1;
}

static int l_vec2_ceil(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::ceil(a), 2);
    return 1;
}

static int l_vec2_round(lua_State* L)
{
    const auto a = LuaCheckVec2(L, 1);
    LuaReturnVec2(L, ansl::vec2::round(a), 2);
    return 1;
}

// -------- vec3 (minimal) --------
static int l_vec3_vec3(lua_State* L)
{
    LuaPushVec3(L, {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)});
    return 1;
}

static int l_vec3_dot(lua_State* L)
{
    const auto a = LuaCheckVec3(L, 1);
    const auto b = LuaCheckVec3(L, 2);
    lua_pushnumber(L, ansl::vec3::dot(a, b));
    return 1;
}

static int l_vec3_length(lua_State* L)
{
    lua_pushnumber(L, ansl::vec3::length(LuaCheckVec3(L, 1)));
    return 1;
}

// -------- sdf --------
static int l_sdf_sdCircle(lua_State* L)
{
    const auto p = LuaCheckVec2(L, 1);
    const double r = luaL_checknumber(L, 2);
    lua_pushnumber(L, ansl::sdf::sdCircle(p, r));
    return 1;
}

static int l_sdf_sdBox(lua_State* L)
{
    const auto p = LuaCheckVec2(L, 1);
    const auto size = LuaCheckVec2(L, 2);
    lua_pushnumber(L, ansl::sdf::sdBox(p, size));
    return 1;
}

static int l_sdf_sdSegment(lua_State* L)
{
    const auto p = LuaCheckVec2(L, 1);
    const auto a = LuaCheckVec2(L, 2);
    const auto b = LuaCheckVec2(L, 3);
    const double thickness = luaL_checknumber(L, 4);
    lua_pushnumber(L, ansl::sdf::sdSegment(p, a, b, thickness));
    return 1;
}

static int l_sdf_opSmoothUnion(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opSmoothUnion(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_opSmoothSubtraction(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opSmoothSubtraction(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_opSmoothIntersection(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opSmoothIntersection(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

// -------- color (xterm-256 index API) --------
// Lua-idiomatic contract for the editor:
// - colors are xterm-256 indices (0..255)
// - nil means "unset"
// - no alpha channel; all palette colors are opaque
static int l_color_rgb(lua_State* L)
{
    const int r = (int)std::llround(luaL_checknumber(L, 1));
    const int g = (int)std::llround(luaL_checknumber(L, 2));
    const int b = (int)std::llround(luaL_checknumber(L, 3));
    const int idx = xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                          (std::uint8_t)std::clamp(g, 0, 255),
                                          (std::uint8_t)std::clamp(b, 0, 255));
    lua_pushinteger(L, idx);
    return 1;
}

static std::string HexByte(int v)
{
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.push_back(hexd[(v >> 4) & 0xF]);
    out.push_back(hexd[v & 0xF]);
    return out;
}

static int l_color_rgb2hex(lua_State* L)
{
    // rgb2hex(idx) -> "#RRGGBB"
    const int idx = (int)luaL_checkinteger(L, 1);
    const xterm256::Rgb c = xterm256::RgbForIndex(idx);
    std::string out = "#";
    out += HexByte(c.r);
    out += HexByte(c.g);
    out += HexByte(c.b);
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

static int l_color_hex(lua_State* L)
{
    // hex("#RRGGBB") -> nearest xterm-256 index
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    if (!s || len < 6)
        return luaL_error(L, "hex() expects '#RRGGBB' or 'RRGGBB'");

    std::string str(s, s + len);
    if (!str.empty() && str[0] == '#')
        str.erase(0, 1);
    if (str.size() != 6)
        return luaL_error(L, "hex() expects 6 hex digits (RRGGBB)");

    auto byte = [&](int off) -> int {
        return (int)std::strtoul(str.substr((size_t)off, 2).c_str(), nullptr, 16);
    };
    const int r = std::clamp(byte(0), 0, 255);
    const int g = std::clamp(byte(2), 0, 255);
    const int b = std::clamp(byte(4), 0, 255);
    const int idx = xterm256::NearestIndex((std::uint8_t)r, (std::uint8_t)g, (std::uint8_t)b);
    lua_pushinteger(L, idx);
    return 1;
}

static int l_color_rgb2gray(lua_State* L)
{
    // rgb2gray(idx) -> 0..1
    const int idx = (int)luaL_checkinteger(L, 1);
    const xterm256::Rgb c = xterm256::RgbForIndex(idx);
    const double gray = std::llround((double)c.r * 0.2126 + (double)c.g * 0.7152 + (double)c.b * 0.0722) / 255.0;
    lua_pushnumber(L, gray);
    return 1;
}

static int l_color_css(lua_State* L)
{
    // css(idx) -> "rgb(r,g,b)"
    const int idx = (int)luaL_checkinteger(L, 1);
    const xterm256::Rgb c = xterm256::RgbForIndex(idx);
    const std::string out =
        "rgb(" + std::to_string((int)c.r) + "," + std::to_string((int)c.g) + "," + std::to_string((int)c.b) + ")";
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

static int l_color_rgb2css(lua_State* L)
{
    // rgb2css is an alias of css in this palette-index API.
    return l_color_css(L);
}

static int l_color_int2rgb(lua_State* L)
{
    // int2rgb(idx) -> {r,g,b}
    const int idx = (int)luaL_checkinteger(L, 1);
    const xterm256::Rgb c = xterm256::RgbForIndex(idx);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, c.r); lua_setfield(L, -2, "r");
    lua_pushinteger(L, c.g); lua_setfield(L, -2, "g");
    lua_pushinteger(L, c.b); lua_setfield(L, -2, "b");
    return 1;
}

static int l_color_is(lua_State* L)
{
    if (!lua_isnumber(L, 1))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    const int idx = (int)lua_tointeger(L, 1);
    lua_pushboolean(L, (idx >= 0 && idx <= 255) ? 1 : 0);
    return 1;
}

static int l_color_rgb_of(lua_State* L)
{
    // rgb_of(idx) -> r,g,b
    const int idx = (int)luaL_checkinteger(L, 1);
    const xterm256::Rgb c = xterm256::RgbForIndex(idx);
    lua_pushinteger(L, c.r);
    lua_pushinteger(L, c.g);
    lua_pushinteger(L, c.b);
    return 3;
}

static void SetFuncs(lua_State* L, const luaL_Reg* regs)
{
    luaL_setfuncs(L, regs, 0);
}

// -------- string (minimal) --------
static int l_string_utf8chars(lua_State* L)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::vector<char32_t> cps;
    ansl::utf8::decode_to_codepoints(s, len, cps);
    lua_createtable(L, (int)cps.size(), 0);
    for (size_t i = 0; i < cps.size(); ++i)
    {
        const std::string ch = ansl::utf8::encode(cps[i]);
        lua_pushlstring(L, ch.data(), ch.size());
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int l_string_measure(lua_State* L)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const auto m = ansl::text::measure_utf8(s, len);
    lua_createtable(L, 0, 3);
    lua_pushlstring(L, s, len); lua_setfield(L, -2, "text");
    lua_pushinteger(L, m.numLines); lua_setfield(L, -2, "numLines");
    lua_pushinteger(L, m.maxWidth); lua_setfield(L, -2, "maxWidth");
    return 1;
}

static int l_string_wrap(lua_State* L)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const int width = (int)luaL_optinteger(L, 2, 0);
    const auto w = ansl::text::wrap_utf8(s, len, width);
    lua_createtable(L, 0, 3);
    lua_pushlstring(L, w.text.c_str(), w.text.size()); lua_setfield(L, -2, "text");
    lua_pushinteger(L, w.numLines); lua_setfield(L, -2, "numLines");
    lua_pushinteger(L, w.maxWidth); lua_setfield(L, -2, "maxWidth");
    return 1;
}

// -------- buffer (portable 2D-on-1D helpers) --------
static bool BufferIndex(int x, int y, int cols, int rows, lua_Integer& outIdx1)
{
    if (cols <= 0 || rows <= 0)
        return false;
    if (x < 0 || x >= cols)
        return false;
    if (y < 0 || y >= rows)
        return false;
    outIdx1 = (lua_Integer)(x + y * cols) + 1;
    return true;
}

static void ShallowCopyTable(lua_State* L, int src_idx)
{
    src_idx = LuaAbsIndex(L, src_idx);
    lua_newtable(L); // dst
    lua_pushnil(L);
    while (lua_next(L, src_idx) != 0)
    {
        lua_pushvalue(L, -2); // key
        lua_pushvalue(L, -2); // value
        lua_settable(L, -5);  // dst[key]=value
        lua_pop(L, 1);        // pop value, keep key for lua_next
    }
}

static int l_buffer_get(lua_State* L)
{
    const int x = (int)luaL_checkinteger(L, 1);
    const int y = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 4);
    const int rows = (int)luaL_checkinteger(L, 5);

    lua_Integer idx1 = 0;
    if (!BufferIndex(x, y, cols, rows, idx1))
    {
        lua_newtable(L); // {} (JS-style "empty object")
        return 1;
    }
    lua_rawgeti(L, 3, idx1);
    return 1;
}

static int l_buffer_set(lua_State* L)
{
    // set(val, x, y, target, cols, rows)
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 5);
    const int rows = (int)luaL_checkinteger(L, 6);

    lua_Integer idx1 = 0;
    if (!BufferIndex(x, y, cols, rows, idx1))
        return 0;

    lua_pushvalue(L, 1);
    lua_rawseti(L, 4, idx1);
    return 0;
}

static int l_buffer_merge(lua_State* L)
{
    // merge(val, x, y, target, cols, rows)
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 5);
    const int rows = (int)luaL_checkinteger(L, 6);

    lua_Integer idx1 = 0;
    if (!BufferIndex(x, y, cols, rows, idx1))
        return 0;

    // base = existing cell
    lua_rawgeti(L, 4, idx1);
    const bool existing_is_table = lua_istable(L, -1);
    const bool existing_is_nil = lua_isnil(L, -1);

    if (existing_is_table)
    {
        ShallowCopyTable(L, -1); // pushes new table
    }
    else
    {
        lua_newtable(L);
        if (!existing_is_nil)
        {
            // char = existing
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, "char");
        }
    }

    // stack now: existing, out_table
    const int out_idx = LuaAbsIndex(L, -1);

    if (lua_istable(L, 1))
    {
        lua_pushnil(L);
        while (lua_next(L, 1) != 0)
        {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_settable(L, out_idx);
            lua_pop(L, 1);
        }
    }
    else
    {
        lua_pushvalue(L, 1);
        lua_setfield(L, out_idx, "char");
    }

    lua_rawseti(L, 4, idx1); // pops out_table, sets target[idx]=out_table
    lua_pop(L, 1);           // pop existing
    return 0;
}

static int l_buffer_setRect(lua_State* L)
{
    // setRect(val, x, y, w, h, target, cols, rows)
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    const int w = (int)luaL_checkinteger(L, 4);
    const int h = (int)luaL_checkinteger(L, 5);
    luaL_checktype(L, 6, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 7);
    const int rows = (int)luaL_checkinteger(L, 8);

    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
        {
            lua_Integer idx1 = 0;
            if (!BufferIndex(i, j, cols, rows, idx1))
                continue;
            lua_pushvalue(L, 1);
            lua_rawseti(L, 6, idx1);
        }
    return 0;
}

static int l_buffer_mergeRect(lua_State* L)
{
    // mergeRect(val, x, y, w, h, target, cols, rows)
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    const int w = (int)luaL_checkinteger(L, 4);
    const int h = (int)luaL_checkinteger(L, 5);
    luaL_checktype(L, 6, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 7);
    const int rows = (int)luaL_checkinteger(L, 8);

    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
        {
            // call merge(val, i, j, target, cols, rows)
            lua_pushcfunction(L, l_buffer_merge);
            lua_pushvalue(L, 1);
            lua_pushinteger(L, i);
            lua_pushinteger(L, j);
            lua_pushvalue(L, 6);
            lua_pushinteger(L, cols);
            lua_pushinteger(L, rows);
            lua_call(L, 6, 0);
        }
    return 0;
}

static int l_buffer_mergeText(lua_State* L)
{
    // mergeText(textObjOrString, x, y, target, cols, rows)
    const int x0 = (int)luaL_checkinteger(L, 2);
    const int y0 = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    const int cols = (int)luaL_checkinteger(L, 5);
    const int rows = (int)luaL_checkinteger(L, 6);

    std::string text;
    bool has_merge_obj = false;
    int merge_obj_idx = 0;

    if (lua_istable(L, 1))
    {
        lua_getfield(L, 1, "text");
        size_t tlen = 0;
        const char* ts = luaL_checklstring(L, -1, &tlen);
        text.assign(ts, ts + tlen);
        lua_pop(L, 1);

        // build merge obj as shallow copy excluding "text"
        lua_newtable(L);
        const int out = LuaAbsIndex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, 1) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                const char* key = lua_tostring(L, -2);
                if (key && std::string(key) == "text")
                {
                    lua_pop(L, 1);
                    continue;
                }
            }
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_settable(L, out);
            lua_pop(L, 1);
        }
        has_merge_obj = true;
        merge_obj_idx = LuaAbsIndex(L, -1);
    }
    else
    {
        size_t tlen = 0;
        const char* ts = luaL_checklstring(L, 1, &tlen);
        text.assign(ts, ts + tlen);
    }

    std::vector<char32_t> cps;
    ansl::utf8::decode_to_codepoints(text.c_str(), text.size(), cps);

    int col = x0;
    int row = y0;
    int lastCol = x0;
    int lastRow = y0;
    int lineLen = 0;

    lua_newtable(L); // wrapInfo
    const int wrapInfoIdx = LuaAbsIndex(L, -1);
    int wrapLine = 0;

    auto finish_line = [&]() {
        // push {first=..., last=...}
        lua_createtable(L, 0, 2);
        lua_pushcfunction(L, l_buffer_get);
        lua_pushinteger(L, x0);
        lua_pushinteger(L, row);
        lua_pushvalue(L, 4);
        lua_pushinteger(L, cols);
        lua_pushinteger(L, rows);
        lua_call(L, 5, 1);
        lua_setfield(L, -2, "first");

        lua_pushcfunction(L, l_buffer_get);
        lua_pushinteger(L, x0 + std::max(0, lineLen - 1));
        lua_pushinteger(L, row);
        lua_pushvalue(L, 4);
        lua_pushinteger(L, cols);
        lua_pushinteger(L, rows);
        lua_call(L, 5, 1);
        lua_setfield(L, -2, "last");

        lua_rawseti(L, wrapInfoIdx, (lua_Integer)wrapLine + 1);
        wrapLine++;
        lineLen = 0;
    };

    for (char32_t cp : cps)
    {
        if (cp == U'\n')
        {
            finish_line();
            row++;
            col = x0;
            continue;
        }

        if (col < x0 + cols) // rough guard; merge handles exact bounds anyway
        {
            // build val table: { char = <utf8>, ...merge_obj }
            lua_newtable(L);
            const int cellIdx = LuaAbsIndex(L, -1);
            const std::string ch = ansl::utf8::encode(cp);
            lua_pushlstring(L, ch.data(), ch.size());
            lua_setfield(L, cellIdx, "char");

            if (has_merge_obj)
            {
                lua_pushnil(L);
                while (lua_next(L, merge_obj_idx) != 0)
                {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_settable(L, cellIdx);
                    lua_pop(L, 1);
                }
            }

            // merge(cell, col, row, target, cols, rows)
            lua_pushcfunction(L, l_buffer_merge);
            lua_pushvalue(L, cellIdx);
            lua_pushinteger(L, col);
            lua_pushinteger(L, row);
            lua_pushvalue(L, 4);
            lua_pushinteger(L, cols);
            lua_pushinteger(L, rows);
            lua_call(L, 6, 0);

            lua_pop(L, 1); // pop cell table
        }

        lastCol = col;
        lastRow = row;
        col++;
        lineLen++;
    }
    finish_line();

    // build return { offset={col,row}, wrapInfo=... }
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, lastCol);
    lua_setfield(L, -2, "col");
    lua_pushinteger(L, lastRow);
    lua_setfield(L, -2, "row");
    lua_setfield(L, -2, "offset");

    lua_pushvalue(L, wrapInfoIdx);
    lua_setfield(L, -2, "wrapInfo");

    // stack currently: wrapInfo, ret (and maybe merge_obj)
    lua_remove(L, wrapInfoIdx); // remove wrapInfo, leaving ret
    if (has_merge_obj)
        lua_remove(L, merge_obj_idx);
    return 1;
}

static const luaL_Reg string_fns[] = {
    {"utf8chars", l_string_utf8chars},
    {"measure", l_string_measure},
    {"wrap", l_string_wrap},
    {nullptr, nullptr},
};

static const luaL_Reg buffer_fns[] = {
    {"get", l_buffer_get},
    {"set", l_buffer_set},
    {"merge", l_buffer_merge},
    {"setRect", l_buffer_setRect},
    {"mergeRect", l_buffer_mergeRect},
    {"mergeText", l_buffer_mergeText},
    {nullptr, nullptr},
};
} // namespace

extern "C" int luaopen_ansl(lua_State* L)
{
    // Return ansl table
    lua_createtable(L, 0, 2);

    lua_pushliteral(L, "1.1");
    lua_setfield(L, -2, "version");

    // num
    lua_createtable(L, 0, 0);
    static const luaL_Reg num_fns[] = {
        {"map", l_num_map},
        {"fract", l_num_fract},
        {"clamp", l_num_clamp},
        {"sign", l_num_sign},
        {"mix", l_num_mix},
        {"step", l_num_step},
        {"smoothstep", l_num_smoothstep},
        {"smootherstep", l_num_smootherstep},
        {"mod", l_num_mod},
        {nullptr, nullptr},
    };
    SetFuncs(L, num_fns);
    lua_setfield(L, -2, "num");

    // vec2
    lua_createtable(L, 0, 0);
    static const luaL_Reg vec2_fns[] = {
        {"vec2", l_vec2_vec2},
        {"copy", l_vec2_copy},
        {"add", l_vec2_add},
        {"sub", l_vec2_sub},
        {"mul", l_vec2_mul},
        {"div", l_vec2_div},
        {"addN", l_vec2_addN},
        {"subN", l_vec2_subN},
        {"mulN", l_vec2_mulN},
        {"divN", l_vec2_divN},
        {"dot", l_vec2_dot},
        {"length", l_vec2_length},
        {"lengthSq", l_vec2_lengthSq},
        {"dist", l_vec2_dist},
        {"distSq", l_vec2_distSq},
        {"norm", l_vec2_norm},
        {"neg", l_vec2_neg},
        {"rot", l_vec2_rot},
        {"mix", l_vec2_mix},
        {"abs", l_vec2_abs},
        {"max", l_vec2_max},
        {"min", l_vec2_min},
        {"fract", l_vec2_fract},
        {"floor", l_vec2_floor},
        {"ceil", l_vec2_ceil},
        {"round", l_vec2_round},
        {nullptr, nullptr},
    };
    SetFuncs(L, vec2_fns);
    lua_setfield(L, -2, "vec2");

    // vec3
    lua_createtable(L, 0, 0);
    static const luaL_Reg vec3_fns[] = {
        {"vec3", l_vec3_vec3},
        {"copy", [](lua_State* L) -> int { LuaReturnVec3(L, LuaCheckVec3(L, 1), 2); return 1; }},
        {"add", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::add(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3); return 1; }},
        {"sub", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::sub(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3); return 1; }},
        {"mul", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::mul(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3); return 1; }},
        {"div", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::div(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3); return 1; }},
        {"addN", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::addN(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)), 3); return 1; }},
        {"subN", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::subN(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)), 3); return 1; }},
        {"mulN", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::mulN(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)), 3); return 1; }},
        {"divN", [](lua_State* L) -> int { LuaReturnVec3(L, ansl::vec3::divN(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)), 3); return 1; }},
        {"dot", l_vec3_dot},
        {"length", l_vec3_length},
        {nullptr, nullptr},
    };
    SetFuncs(L, vec3_fns);
    lua_setfield(L, -2, "vec3");

    // sdf
    lua_createtable(L, 0, 0);
    static const luaL_Reg sdf_fns[] = {
        {"sdCircle", l_sdf_sdCircle},
        {"sdBox", l_sdf_sdBox},
        {"sdSegment", l_sdf_sdSegment},
        {"opSmoothUnion", l_sdf_opSmoothUnion},
        {"opSmoothSubtraction", l_sdf_opSmoothSubtraction},
        {"opSmoothIntersection", l_sdf_opSmoothIntersection},
        {nullptr, nullptr},
    };
    SetFuncs(L, sdf_fns);
    lua_setfield(L, -2, "sdf");

    // color (xterm-256 indices)
    lua_createtable(L, 0, 0);
    static const luaL_Reg color_fns[] = {
        {"rgb", l_color_rgb},         // (r,g,b) -> idx
        {"hex", l_color_hex},         // ("#RRGGBB") -> idx
        {"is", l_color_is},           // (idx) -> bool
        {"css", l_color_css},         // (idx) -> "rgb(r,g,b)"
        {"rgb2hex", l_color_rgb2hex}, // (idx) -> "#RRGGBB"
        {"rgb2css", l_color_rgb2css}, // alias of css
        {"rgb2gray", l_color_rgb2gray}, // (idx) -> 0..1
        {"int2rgb", l_color_int2rgb}, // (idx) -> {r,g,b}
        {"rgb_of", l_color_rgb_of},   // (idx) -> r,g,b
        {nullptr, nullptr},
    };
    SetFuncs(L, color_fns);

    // Export ansl.color.xterm = { [0]=0, [1]=1, ... } (0-based keys for convenience)
    // and a minimal ANSI-16 name map.
    lua_createtable(L, 0, 256);
    for (int i = 0; i < 256; ++i)
    {
        lua_pushinteger(L, i);
        lua_rawseti(L, -2, i); // key=i (LuaJIT allows 0 integer keys); value=i
    }
    lua_setfield(L, -2, "xterm");

    lua_createtable(L, 0, 16);
    lua_pushinteger(L, 0); lua_setfield(L, -2, "black");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "red");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "green");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "yellow");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "blue");
    lua_pushinteger(L, 5); lua_setfield(L, -2, "magenta");
    lua_pushinteger(L, 6); lua_setfield(L, -2, "cyan");
    lua_pushinteger(L, 7); lua_setfield(L, -2, "white");
    lua_pushinteger(L, 8); lua_setfield(L, -2, "bright_black");
    lua_pushinteger(L, 9); lua_setfield(L, -2, "bright_red");
    lua_pushinteger(L, 10); lua_setfield(L, -2, "bright_green");
    lua_pushinteger(L, 11); lua_setfield(L, -2, "bright_yellow");
    lua_pushinteger(L, 12); lua_setfield(L, -2, "bright_blue");
    lua_pushinteger(L, 13); lua_setfield(L, -2, "bright_magenta");
    lua_pushinteger(L, 14); lua_setfield(L, -2, "bright_cyan");
    lua_pushinteger(L, 15); lua_setfield(L, -2, "bright_white");
    lua_setfield(L, -2, "ansi16");

    lua_setfield(L, -2, "color");

    // buffer (portable)
    lua_createtable(L, 0, 0);
    SetFuncs(L, buffer_fns);
    lua_setfield(L, -2, "buffer");
    // drawbox: host-specific (depends on styling + higher-level layout); keep as stub for now
    lua_createtable(L, 0, 0);
    lua_setfield(L, -2, "drawbox");
    // string (minimal, plus UTF-8 helpers for LuaJIT)
    lua_createtable(L, 0, 0);
    SetFuncs(L, string_fns);
    lua_setfield(L, -2, "string");

    return 1;
}


