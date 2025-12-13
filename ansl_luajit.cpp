#include "ansl_native.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

#include <string>
#include <algorithm>
#include <cmath>

namespace
{
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

// -------- color (minimal helpers only) --------
static int l_color_rgb(lua_State* L)
{
    const double r = luaL_checknumber(L, 1);
    const double g = luaL_checknumber(L, 2);
    const double b = luaL_checknumber(L, 3);
    const double a = luaL_optnumber(L, 4, 1.0);
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, r); lua_setfield(L, -2, "r");
    lua_pushnumber(L, g); lua_setfield(L, -2, "g");
    lua_pushnumber(L, b); lua_setfield(L, -2, "b");
    lua_pushnumber(L, a); lua_setfield(L, -2, "a");
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
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "r");
    const double r = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "g");
    const double g = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "b");
    const double b = luaL_checknumber(L, -1);
    lua_pop(L, 1);

    bool has_a = false;
    double a = 1.0;
    lua_getfield(L, 1, "a");
    if (!lua_isnil(L, -1))
    {
        has_a = true;
        a = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    const int ri = (int)std::llround(r);
    const int gi = (int)std::llround(g);
    const int bi = (int)std::llround(b);
    std::string out = "#";
    out += HexByte(std::clamp(ri, 0, 255));
    out += HexByte(std::clamp(gi, 0, 255));
    out += HexByte(std::clamp(bi, 0, 255));
    if (has_a)
    {
        const int ai = (int)std::llround(a * 255.0);
        out += HexByte(std::clamp(ai, 0, 255));
    }
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

static int l_color_int2rgb(lua_State* L)
{
    const int v = (int)luaL_checkinteger(L, 1);
    const int r = (v >> 16) & 0xff;
    const int g = (v >> 8) & 0xff;
    const int b = (v)&0xff;
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, 1.0); lua_setfield(L, -2, "a");
    lua_pushinteger(L, r); lua_setfield(L, -2, "r");
    lua_pushinteger(L, g); lua_setfield(L, -2, "g");
    lua_pushinteger(L, b); lua_setfield(L, -2, "b");
    return 1;
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

static const luaL_Reg string_fns[] = {
    {"utf8chars", l_string_utf8chars},
    {nullptr, nullptr},
};
} // namespace

extern "C" int luaopen_ansl(lua_State* L)
{
    // Return ANSL table
    lua_createtable(L, 0, 2);

    lua_pushliteral(L, "1.1");
    lua_setfield(L, -2, "version");

    // modules table
    lua_createtable(L, 0, 8);

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
        {"opSmoothUnion", l_sdf_opSmoothUnion},
        {"opSmoothSubtraction", l_sdf_opSmoothSubtraction},
        {"opSmoothIntersection", l_sdf_opSmoothIntersection},
        {nullptr, nullptr},
    };
    SetFuncs(L, sdf_fns);
    lua_setfield(L, -2, "sdf");

    // color (minimal)
    lua_createtable(L, 0, 0);
    static const luaL_Reg color_fns[] = {
        {"rgb", l_color_rgb},
        {"rgb2hex", l_color_rgb2hex},
        {"int2rgb", l_color_int2rgb},
        {nullptr, nullptr},
    };
    SetFuncs(L, color_fns);
    lua_setfield(L, -2, "color");

    // stubs
    lua_createtable(L, 0, 0); lua_setfield(L, -2, "buffer");
    lua_createtable(L, 0, 0); lua_setfield(L, -2, "drawbox");
    // string (minimal, plus UTF-8 helpers for LuaJIT)
    lua_createtable(L, 0, 0);
    SetFuncs(L, string_fns);
    lua_setfield(L, -2, "string");

    lua_setfield(L, -2, "modules");

    return 1;
}


