#include "ansl/ansl_native.h"

#include "imgui.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

#include <noise/noise.h>
#include <noise/noisegen.h>

#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "core/color_ops.h"
#include "core/color_system.h"
#include "fonts/textmode_font_registry.h"

namespace
{
static inline std::uint8_t QuantizeRgbToPaletteIndex_Quant3dOrExact(phos::color::PaletteInstanceId pal,
                                                                    std::uint8_t r,
                                                                    std::uint8_t g,
                                                                    std::uint8_t b,
                                                                    const phos::color::QuantizePolicy& qp)
{
    // Lua-facing rgb()/hex() are hot in scripts that generate colors per-pixel. Prefer a Quant3D LUT
    // (prebuilt by the host when possible), falling back to the exact deterministic scan path.
    auto& cs = phos::color::GetColorSystem();
    constexpr std::uint8_t kBits = 5;
    const auto qlut = cs.Luts().GetOrBuildQuant3d(cs.Palettes(), pal, kBits, qp);
    if (qlut && qlut->bits == kBits && !qlut->table.empty())
    {
        const std::size_t side = (std::size_t)1u << kBits;
        const std::size_t bin_size = 256u / side;
        const std::size_t rx = std::min<std::size_t>(side - 1u, (std::size_t)r / bin_size);
        const std::size_t gy = std::min<std::size_t>(side - 1u, (std::size_t)g / bin_size);
        const std::size_t bz = std::min<std::size_t>(side - 1u, (std::size_t)b / bin_size);
        const std::size_t flat = (bz * side + gy) * side + rx;
        if (flat < qlut->table.size())
            return qlut->table[flat];
    }
    return phos::color::ColorOps::NearestIndexRgb(cs.Palettes(), pal, r, g, b, qp);
}

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

static int l_num_mod_glsl(lua_State* L)
{
    lua_pushnumber(L, ansl::num::mod_glsl(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
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

static int l_vec3_lengthSq(lua_State* L)
{
    lua_pushnumber(L, ansl::vec3::lengthSq(LuaCheckVec3(L, 1)));
    return 1;
}

static int l_vec3_norm(lua_State* L)
{
    LuaReturnVec3(L, ansl::vec3::norm(LuaCheckVec3(L, 1)), 2);
    return 1;
}

static int l_vec3_abs(lua_State* L)
{
    LuaReturnVec3(L, ansl::vec3::abs(LuaCheckVec3(L, 1)), 2);
    return 1;
}

static int l_vec3_max(lua_State* L)
{
    LuaReturnVec3(L, ansl::vec3::max(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3);
    return 1;
}

static int l_vec3_min(lua_State* L)
{
    LuaReturnVec3(L, ansl::vec3::min(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)), 3);
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

// ---- sdf (hard boolean ops) ----
static int l_sdf_opUnion(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opUnion(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_sdf_opIntersection(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opIntersection(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_sdf_opDifference(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::opDifference(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

// ---- sdf.hg primitives ----
static int l_sdf_fSphere(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fSphere(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_sdf_fPlane(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fPlane(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fBoxCheap(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fBoxCheap(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)));
    return 1;
}

static int l_sdf_fBox(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fBox(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2)));
    return 1;
}

static int l_sdf_fBox2Cheap(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fBox2Cheap(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2)));
    return 1;
}

static int l_sdf_fBox2(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fBox2(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2)));
    return 1;
}

static int l_sdf_fCorner(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fCorner(LuaCheckVec2(L, 1)));
    return 1;
}

static int l_sdf_fBlob(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fBlob(LuaCheckVec3(L, 1)));
    return 1;
}

static int l_sdf_fCylinder(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fCylinder(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fCapsule(lua_State* L)
{
    // Overload:
    // - fCapsule(p:vec3, r:number, c:number)
    // - fCapsule(p:vec3, a:vec3, b:vec3, r:number)
    if (lua_gettop(L) >= 4 && lua_istable(L, 2))
    {
        lua_pushnumber(L, ansl::sdf::hg::fCapsule(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2), LuaCheckVec3(L, 3), luaL_checknumber(L, 4)));
        return 1;
    }
    lua_pushnumber(L, ansl::sdf::hg::fCapsule(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fLineSegment(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fLineSegment(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2), LuaCheckVec3(L, 3)));
    return 1;
}

static int l_sdf_fTorus(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fTorus(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fCircle(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fCircle(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_sdf_fDisc(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fDisc(LuaCheckVec3(L, 1), luaL_checknumber(L, 2)));
    return 1;
}

static int l_sdf_fHexagonCircumcircle(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fHexagonCircumcircle(LuaCheckVec3(L, 1), LuaCheckVec2(L, 2)));
    return 1;
}

static int l_sdf_fHexagonIncircle(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fHexagonIncircle(LuaCheckVec3(L, 1), LuaCheckVec2(L, 2)));
    return 1;
}

static int l_sdf_fCone(lua_State* L)
{
    lua_pushnumber(L, ansl::sdf::hg::fCone(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fGDF(lua_State* L)
{
    // Overload:
    // - fGDF(p, r, begin, end)
    // - fGDF(p, r, e, begin, end)
    const int n = lua_gettop(L);
    if (n == 4)
    {
        lua_pushnumber(L, ansl::sdf::hg::fGDF(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4)));
        return 1;
    }
    lua_pushnumber(L, ansl::sdf::hg::fGDF(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5)));
    return 1;
}

static int l_sdf_fOctahedron(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 2) { lua_pushnumber(L, ansl::sdf::hg::fOctahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2))); return 1; }
    lua_pushnumber(L, ansl::sdf::hg::fOctahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fDodecahedron(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 2) { lua_pushnumber(L, ansl::sdf::hg::fDodecahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2))); return 1; }
    lua_pushnumber(L, ansl::sdf::hg::fDodecahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fIcosahedron(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 2) { lua_pushnumber(L, ansl::sdf::hg::fIcosahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2))); return 1; }
    lua_pushnumber(L, ansl::sdf::hg::fIcosahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fTruncatedOctahedron(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 2) { lua_pushnumber(L, ansl::sdf::hg::fTruncatedOctahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2))); return 1; }
    lua_pushnumber(L, ansl::sdf::hg::fTruncatedOctahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

static int l_sdf_fTruncatedIcosahedron(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 2) { lua_pushnumber(L, ansl::sdf::hg::fTruncatedIcosahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2))); return 1; }
    lua_pushnumber(L, ansl::sdf::hg::fTruncatedIcosahedron(LuaCheckVec3(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)));
    return 1;
}

// ---- sdf.hg domain ops ----
static int l_sdf_pR(lua_State* L)
{
    LuaReturnVec2(L, ansl::sdf::hg::pR(LuaCheckVec2(L, 1), luaL_checknumber(L, 2)), 3);
    return 1;
}

static int l_sdf_pR45(lua_State* L)
{
    LuaReturnVec2(L, ansl::sdf::hg::pR45(LuaCheckVec2(L, 1)), 2);
    return 1;
}

static int l_sdf_pMod1(lua_State* L)
{
    const auto r = ansl::sdf::hg::pMod1(luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    lua_pushnumber(L, r.p);
    lua_pushnumber(L, r.c);
    return 2;
}

static int l_sdf_pModMirror1(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModMirror1(luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    lua_pushnumber(L, r.p);
    lua_pushnumber(L, r.c);
    return 2;
}

static int l_sdf_pModSingle1(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModSingle1(luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    lua_pushnumber(L, r.p);
    lua_pushnumber(L, r.c);
    return 2;
}

static int l_sdf_pModInterval1(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModInterval1(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4));
    lua_pushnumber(L, r.p);
    lua_pushnumber(L, r.c);
    return 2;
}

static int l_sdf_pModPolar(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModPolar(LuaCheckVec2(L, 1), luaL_checknumber(L, 2));
    LuaReturnVec2(L, r.first, 3);
    lua_pushnumber(L, r.second);
    return 2;
}

static int l_sdf_pMod2(lua_State* L)
{
    const auto r = ansl::sdf::hg::pMod2(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2));
    LuaReturnVec2(L, r.p, 3);
    LuaReturnVec2(L, r.c, 4);
    return 2;
}

static int l_sdf_pModMirror2(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModMirror2(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2));
    LuaReturnVec2(L, r.p, 3);
    LuaReturnVec2(L, r.c, 4);
    return 2;
}

static int l_sdf_pModGrid2(lua_State* L)
{
    const auto r = ansl::sdf::hg::pModGrid2(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2));
    LuaReturnVec2(L, r.p, 3);
    LuaReturnVec2(L, r.c, 4);
    return 2;
}

static int l_sdf_pMod3(lua_State* L)
{
    const auto r = ansl::sdf::hg::pMod3(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2));
    LuaReturnVec3(L, r.p, 3);
    LuaReturnVec3(L, r.c, 4);
    return 2;
}

static int l_sdf_pMirror(lua_State* L)
{
    const auto r = ansl::sdf::hg::pMirror(luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    lua_pushnumber(L, r.p);
    lua_pushnumber(L, r.s);
    return 2;
}

static int l_sdf_pMirrorOctant(lua_State* L)
{
    const auto r = ansl::sdf::hg::pMirrorOctant(LuaCheckVec2(L, 1), LuaCheckVec2(L, 2));
    LuaReturnVec2(L, r.p, 3);
    LuaReturnVec2(L, r.s, 4);
    return 2;
}

static int l_sdf_pReflect(lua_State* L)
{
    const auto r = ansl::sdf::hg::pReflect(LuaCheckVec3(L, 1), LuaCheckVec3(L, 2), luaL_checknumber(L, 3));
    LuaReturnVec3(L, r.p, 4);
    lua_pushnumber(L, r.s);
    return 2;
}

// ---- sdf.hg object combination operators ----
static int l_sdf_fOpUnionChamfer(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpUnionChamfer(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpIntersectionChamfer(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpIntersectionChamfer(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpDifferenceChamfer(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpDifferenceChamfer(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpUnionRound(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpUnionRound(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpIntersectionRound(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpIntersectionRound(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpDifferenceRound(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpDifferenceRound(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpUnionColumns(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpUnionColumns(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpDifferenceColumns(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpDifferenceColumns(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpIntersectionColumns(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpIntersectionColumns(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpUnionStairs(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpUnionStairs(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpIntersectionStairs(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpIntersectionStairs(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpDifferenceStairs(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpDifferenceStairs(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpUnionSoft(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpUnionSoft(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpPipe(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpPipe(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpEngrave(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpEngrave(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3))); return 1; }
static int l_sdf_fOpGroove(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpGroove(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }
static int l_sdf_fOpTongue(lua_State* L) { lua_pushnumber(L, ansl::sdf::hg::fOpTongue(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; }

// -------- color (active palette index API) --------
// Lua-idiomatic contract for the editor:
// - colors are palette indices in the active canvas palette (0..paletteSize-1)
// - nil means "unset"
// - no alpha channel; all palette colors are opaque
namespace
{
static phos::color::PaletteInstanceId LuaGetActivePaletteId(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "phosphor.active_palette_instance_id");
    phos::color::PaletteInstanceId pal = {};
    if (lua_isnumber(L, -1))
        pal.v = (std::uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    auto& cs = phos::color::GetColorSystem();
    if (pal.v == 0)
        pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    return pal;
}

static const phos::color::Palette* LuaGetActivePalette(lua_State* L, phos::color::PaletteInstanceId& out_pal)
{
    auto& cs = phos::color::GetColorSystem();
    out_pal = LuaGetActivePaletteId(L);
    if (const phos::color::Palette* p = cs.Palettes().Get(out_pal))
        return p;
    out_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
    return cs.Palettes().Get(out_pal);
}

static int LuaActivePaletteSizeOrZero(lua_State* L)
{
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p)
        return 0;
    return (int)p->rgb.size();
}
} // namespace

static int l_color_rgb(lua_State* L)
{
    const int r = (int)std::llround(luaL_checknumber(L, 1));
    const int g = (int)std::llround(luaL_checknumber(L, 2));
    const int b = (int)std::llround(luaL_checknumber(L, 3));
    const phos::color::PaletteInstanceId pal = LuaGetActivePaletteId(L);
    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    const int idx = (int)QuantizeRgbToPaletteIndex_Quant3dOrExact(pal,
                                                                 (std::uint8_t)std::clamp(r, 0, 255),
                                                                 (std::uint8_t)std::clamp(g, 0, 255),
                                                                 (std::uint8_t)std::clamp(b, 0, 255),
                                                                 qp);
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
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p || idx < 0 || idx >= (int)p->rgb.size())
        return luaL_error(L, "rgb2hex() expects an index in the active palette (0..paletteSize-1)");
    const phos::color::Rgb8 c = p->rgb[(size_t)idx];
    std::string out = "#";
    out += HexByte(c.r);
    out += HexByte(c.g);
    out += HexByte(c.b);
    lua_pushlstring(L, out.c_str(), out.size());
    return 1;
}

static int l_color_hex(lua_State* L)
{
    // hex("#RRGGBB") -> nearest index in the active palette
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
    const phos::color::PaletteInstanceId pal = LuaGetActivePaletteId(L);
    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    const int idx = (int)QuantizeRgbToPaletteIndex_Quant3dOrExact(pal,
                                                                 (std::uint8_t)r,
                                                                 (std::uint8_t)g,
                                                                 (std::uint8_t)b,
                                                                 qp);
    lua_pushinteger(L, idx);
    return 1;
}

static int l_color_rgb2gray(lua_State* L)
{
    // rgb2gray(idx) -> 0..1
    const int idx = (int)luaL_checkinteger(L, 1);
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p || idx < 0 || idx >= (int)p->rgb.size())
        return luaL_error(L, "rgb2gray() expects an index in the active palette (0..paletteSize-1)");
    const phos::color::Rgb8 c = p->rgb[(size_t)idx];
    const double gray = std::llround((double)c.r * 0.2126 + (double)c.g * 0.7152 + (double)c.b * 0.0722) / 255.0;
    lua_pushnumber(L, gray);
    return 1;
}

static int l_color_css(lua_State* L)
{
    // css(idx) -> "rgb(r,g,b)"
    const int idx = (int)luaL_checkinteger(L, 1);
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p || idx < 0 || idx >= (int)p->rgb.size())
        return luaL_error(L, "css() expects an index in the active palette (0..paletteSize-1)");
    const phos::color::Rgb8 c = p->rgb[(size_t)idx];
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
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p || idx < 0 || idx >= (int)p->rgb.size())
        return luaL_error(L, "int2rgb() expects an index in the active palette (0..paletteSize-1)");
    const phos::color::Rgb8 c = p->rgb[(size_t)idx];
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
    const int n = LuaActivePaletteSizeOrZero(L);
    lua_pushboolean(L, (idx >= 0 && idx < n) ? 1 : 0);
    return 1;
}

static int l_color_rgb_of(lua_State* L)
{
    // rgb_of(idx) -> r,g,b
    const int idx = (int)luaL_checkinteger(L, 1);
    phos::color::PaletteInstanceId pal = {};
    const phos::color::Palette* p = LuaGetActivePalette(L, pal);
    if (!p || idx < 0 || idx >= (int)p->rgb.size())
        return luaL_error(L, "rgb_of() expects an index in the active palette (0..paletteSize-1)");
    const phos::color::Rgb8 c = p->rgb[(size_t)idx];
    lua_pushinteger(L, c.r);
    lua_pushinteger(L, c.g);
    lua_pushinteger(L, c.b);
    return 3;
}

static void SetFuncs(lua_State* L, const luaL_Reg* regs)
{
    luaL_setfuncs(L, regs, 0);
}

static int l_color_ansi16_index(lua_State* L)
{
    // __index metamethod for ansl.color.ansi16.*
    // Returns the nearest index in the active palette for the requested VGA16/ANSI16 named color.
    const char* key = luaL_checkstring(L, 2);
    int vga = -1;
    if (std::strcmp(key, "black") == 0) vga = 0;
    else if (std::strcmp(key, "red") == 0) vga = 1;
    else if (std::strcmp(key, "green") == 0) vga = 2;
    else if (std::strcmp(key, "yellow") == 0) vga = 3;
    else if (std::strcmp(key, "blue") == 0) vga = 4;
    else if (std::strcmp(key, "magenta") == 0) vga = 5;
    else if (std::strcmp(key, "cyan") == 0) vga = 6;
    else if (std::strcmp(key, "white") == 0) vga = 7;
    else if (std::strcmp(key, "bright_black") == 0) vga = 8;
    else if (std::strcmp(key, "bright_red") == 0) vga = 9;
    else if (std::strcmp(key, "bright_green") == 0) vga = 10;
    else if (std::strcmp(key, "bright_yellow") == 0) vga = 11;
    else if (std::strcmp(key, "bright_blue") == 0) vga = 12;
    else if (std::strcmp(key, "bright_magenta") == 0) vga = 13;
    else if (std::strcmp(key, "bright_cyan") == 0) vga = 14;
    else if (std::strcmp(key, "bright_white") == 0) vga = 15;

    if (vga < 0)
    {
        lua_pushnil(L);
        return 1;
    }

    auto& cs = phos::color::GetColorSystem();
    const phos::color::PaletteInstanceId vga_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);
    const phos::color::Palette* vga_p = cs.Palettes().Get(vga_pal);
    if (!vga_p || vga_p->rgb.size() < 16)
    {
        lua_pushnil(L);
        return 1;
    }

    const phos::color::Rgb8 c = vga_p->rgb[(size_t)vga];
    const phos::color::PaletteInstanceId pal = LuaGetActivePaletteId(L);
    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    const int idx = (int)phos::color::ColorOps::NearestIndexRgb(cs.Palettes(), pal, c.r, c.g, c.b, qp);
    lua_pushinteger(L, idx);
    return 1;
}

// -------- noise (libnoise) --------
// Lua API:
//   local n = ansl.noise.perlin{ seed=..., frequency=..., lacunarity=..., octaves=..., persistence=..., quality="std" }
//   local v = n:get(x,y,z)   -- number (usually in [-1,1])
//   local v = n:get2(x,y)    -- z=0
//
// Similar constructors: billow{}, ridged{}, voronoi{}.
namespace noise_lua
{
static const char* MT_PERLIN  = "AnslNoisePerlin";
static const char* MT_BILLOW  = "AnslNoiseBillow";
static const char* MT_RIDGED  = "AnslNoiseRidged";
static const char* MT_VORONOI = "AnslNoiseVoronoi";

template <typename T>
static T* Check(lua_State* L, int idx, const char* mt)
{
    void* ud = luaL_checkudata(L, idx, mt);
    return static_cast<T*>(ud);
}

static noise::NoiseQuality ParseQualityFromStack(lua_State* L, int idx, noise::NoiseQuality def = noise::QUALITY_STD)
{
    if (lua_gettop(L) < idx || lua_isnil(L, idx))
        return def;
    if (lua_isnumber(L, idx))
    {
        const int q = (int)lua_tointeger(L, idx);
        if (q <= 0) return noise::QUALITY_FAST;
        if (q == 1) return noise::QUALITY_STD;
        return noise::QUALITY_BEST;
    }
    if (lua_isstring(L, idx))
    {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        const std::string v(s ? s : "", s ? (s + len) : (s ? s : ""));
        if (v == "fast" || v == "FAST" || v == "0") return noise::QUALITY_FAST;
        if (v == "std" || v == "standard" || v == "STD" || v == "1") return noise::QUALITY_STD;
        if (v == "best" || v == "BEST" || v == "2") return noise::QUALITY_BEST;
    }
    return def;
}

static bool GetFieldNumber(lua_State* L, int idx, const char* key, double& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, key);
    if (!lua_isnumber(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = (double)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return true;
}

static bool GetFieldInt(lua_State* L, int idx, const char* key, int& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, key);
    if (!lua_isnumber(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return true;
}

static bool GetFieldBool(lua_State* L, int idx, const char* key, bool& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return true;
}

static bool GetFieldQuality(lua_State* L, int idx, const char* key, noise::NoiseQuality& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = ParseQualityFromStack(L, lua_gettop(L), out);
    lua_pop(L, 1);
    return true;
}

template <typename ModuleT>
static void ApplyCommonFractalOpts(lua_State* L, int opt_idx, ModuleT& m)
{
    int seed = 0;
    if (GetFieldInt(L, opt_idx, "seed", seed))
        m.SetSeed(seed);

    double freq = 0.0;
    if (GetFieldNumber(L, opt_idx, "frequency", freq))
        m.SetFrequency(freq);

    double lac = 0.0;
    if (GetFieldNumber(L, opt_idx, "lacunarity", lac))
        m.SetLacunarity(lac);

    int oct = 0;
    if (GetFieldInt(L, opt_idx, "octaves", oct) || GetFieldInt(L, opt_idx, "octaveCount", oct))
        m.SetOctaveCount(oct);

    noise::NoiseQuality q = m.GetNoiseQuality();
    if (GetFieldQuality(L, opt_idx, "quality", q))
        m.SetNoiseQuality(q);
}

static void ApplyPersistenceOpt(lua_State* L, int opt_idx, noise::module::Perlin& m)
{
    double p = 0.0;
    if (GetFieldNumber(L, opt_idx, "persistence", p))
        m.SetPersistence(p);
}

static void ApplyPersistenceOpt(lua_State* L, int opt_idx, noise::module::Billow& m)
{
    double p = 0.0;
    if (GetFieldNumber(L, opt_idx, "persistence", p))
        m.SetPersistence(p);
}

struct PerlinUD { noise::module::Perlin m; };
struct BillowUD { noise::module::Billow m; };
struct RidgedUD { noise::module::RidgedMulti m; };
struct VoronoiUD { noise::module::Voronoi m; };

static int l_perlin_gc(lua_State* L)  { Check<PerlinUD>(L, 1, MT_PERLIN)->~PerlinUD(); return 0; }
static int l_billow_gc(lua_State* L)  { Check<BillowUD>(L, 1, MT_BILLOW)->~BillowUD(); return 0; }
static int l_ridged_gc(lua_State* L)  { Check<RidgedUD>(L, 1, MT_RIDGED)->~RidgedUD(); return 0; }
static int l_voronoi_gc(lua_State* L) { Check<VoronoiUD>(L, 1, MT_VORONOI)->~VoronoiUD(); return 0; }

static int l_perlin_get(lua_State* L)
{
    const auto* ud = Check<PerlinUD>(L, 1, MT_PERLIN);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)));
    return 1;
}
static int l_perlin_get2(lua_State* L)
{
    const auto* ud = Check<PerlinUD>(L, 1, MT_PERLIN);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), 0.0));
    return 1;
}
static int l_billow_get(lua_State* L)
{
    const auto* ud = Check<BillowUD>(L, 1, MT_BILLOW);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)));
    return 1;
}
static int l_billow_get2(lua_State* L)
{
    const auto* ud = Check<BillowUD>(L, 1, MT_BILLOW);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), 0.0));
    return 1;
}
static int l_ridged_get(lua_State* L)
{
    const auto* ud = Check<RidgedUD>(L, 1, MT_RIDGED);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)));
    return 1;
}
static int l_ridged_get2(lua_State* L)
{
    const auto* ud = Check<RidgedUD>(L, 1, MT_RIDGED);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), 0.0));
    return 1;
}
static int l_voronoi_get(lua_State* L)
{
    const auto* ud = Check<VoronoiUD>(L, 1, MT_VORONOI);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)));
    return 1;
}
static int l_voronoi_get2(lua_State* L)
{
    const auto* ud = Check<VoronoiUD>(L, 1, MT_VORONOI);
    lua_pushnumber(L, ud->m.GetValue(luaL_checknumber(L, 2), luaL_checknumber(L, 3), 0.0));
    return 1;
}

static int l_noise_perlin(lua_State* L)
{
    void* mem = lua_newuserdata(L, sizeof(PerlinUD));
    auto* ud = new (mem) PerlinUD();
    if (lua_gettop(L) >= 1 && lua_istable(L, 1))
    {
        try { ApplyCommonFractalOpts(L, 1, ud->m); ApplyPersistenceOpt(L, 1, ud->m); }
        catch (...) { return luaL_error(L, "ansl.noise.perlin: invalid options"); }
    }
    luaL_setmetatable(L, MT_PERLIN);
    return 1;
}

static int l_noise_billow(lua_State* L)
{
    void* mem = lua_newuserdata(L, sizeof(BillowUD));
    auto* ud = new (mem) BillowUD();
    if (lua_gettop(L) >= 1 && lua_istable(L, 1))
    {
        try { ApplyCommonFractalOpts(L, 1, ud->m); ApplyPersistenceOpt(L, 1, ud->m); }
        catch (...) { return luaL_error(L, "ansl.noise.billow: invalid options"); }
    }
    luaL_setmetatable(L, MT_BILLOW);
    return 1;
}

static int l_noise_ridged(lua_State* L)
{
    void* mem = lua_newuserdata(L, sizeof(RidgedUD));
    auto* ud = new (mem) RidgedUD();
    if (lua_gettop(L) >= 1 && lua_istable(L, 1))
    {
        try { ApplyCommonFractalOpts(L, 1, ud->m); }
        catch (...) { return luaL_error(L, "ansl.noise.ridged: invalid options"); }
    }
    luaL_setmetatable(L, MT_RIDGED);
    return 1;
}

static int l_noise_voronoi(lua_State* L)
{
    void* mem = lua_newuserdata(L, sizeof(VoronoiUD));
    auto* ud = new (mem) VoronoiUD();
    if (lua_gettop(L) >= 1 && lua_istable(L, 1))
    {
        try
        {
            int seed = 0;
            if (GetFieldInt(L, 1, "seed", seed)) ud->m.SetSeed(seed);
            double freq = 0.0;
            if (GetFieldNumber(L, 1, "frequency", freq)) ud->m.SetFrequency(freq);
            double disp = 0.0;
            if (GetFieldNumber(L, 1, "displacement", disp)) ud->m.SetDisplacement(disp);
            bool dist = false;
            if (GetFieldBool(L, 1, "distance", dist) || GetFieldBool(L, 1, "enableDistance", dist))
                ud->m.EnableDistance(dist);
        }
        catch (...) { return luaL_error(L, "ansl.noise.voronoi: invalid options"); }
    }
    luaL_setmetatable(L, MT_VORONOI);
    return 1;
}

// value noise helper (integer lattice)
static int l_noise_value3(lua_State* L)
{
    const int x = (int)luaL_checkinteger(L, 1);
    const int y = (int)luaL_checkinteger(L, 2);
    const int z = (int)luaL_checkinteger(L, 3);
    const int seed = (int)luaL_optinteger(L, 4, 0);
    lua_pushnumber(L, noise::ValueNoise3D(x, y, z, seed));
    return 1;
}

static void EnsureMt(lua_State* L, const char* mt, const luaL_Reg* methods, lua_CFunction gc)
{
    if (luaL_newmetatable(L, mt))
    {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L); // __index
        SetFuncs(L, methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1); // mt
}

static void EnsureNoiseMetatables(lua_State* L)
{
    static const luaL_Reg perlin_methods[] = {
        {"get", l_perlin_get},
        {"get2", l_perlin_get2},
        {nullptr, nullptr},
    };
    static const luaL_Reg billow_methods[] = {
        {"get", l_billow_get},
        {"get2", l_billow_get2},
        {nullptr, nullptr},
    };
    static const luaL_Reg ridged_methods[] = {
        {"get", l_ridged_get},
        {"get2", l_ridged_get2},
        {nullptr, nullptr},
    };
    static const luaL_Reg voronoi_methods[] = {
        {"get", l_voronoi_get},
        {"get2", l_voronoi_get2},
        {nullptr, nullptr},
    };
    EnsureMt(L, MT_PERLIN, perlin_methods, l_perlin_gc);
    EnsureMt(L, MT_BILLOW, billow_methods, l_billow_gc);
    EnsureMt(L, MT_RIDGED, ridged_methods, l_ridged_gc);
    EnsureMt(L, MT_VORONOI, voronoi_methods, l_voronoi_gc);
}

static void PushNoiseModule(lua_State* L)
{
    EnsureNoiseMetatables(L);

    lua_createtable(L, 0, 8); // noise

    lua_pushcfunction(L, l_noise_perlin);  lua_setfield(L, -2, "perlin");
    lua_pushcfunction(L, l_noise_billow);  lua_setfield(L, -2, "billow");
    lua_pushcfunction(L, l_noise_ridged);  lua_setfield(L, -2, "ridged");
    lua_pushcfunction(L, l_noise_voronoi); lua_setfield(L, -2, "voronoi");
    lua_pushcfunction(L, l_noise_value3);  lua_setfield(L, -2, "value3");

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)noise::QUALITY_FAST); lua_setfield(L, -2, "fast");
    lua_pushinteger(L, (lua_Integer)noise::QUALITY_STD);  lua_setfield(L, -2, "std");
    lua_pushinteger(L, (lua_Integer)noise::QUALITY_BEST); lua_setfield(L, -2, "best");
    lua_setfield(L, -2, "quality");
}
} // namespace noise_lua

// -------- sort (glyph brightness / ink coverage) --------
static int l_sort_brightness(lua_State* L)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    const bool ascending = (lua_gettop(L) >= 2) ? (lua_toboolean(L, 2) != 0) : false;

    // Use the app font (Unscii). During script load/compile we may not have a "current"
    // window/font yet, so fall back to the first atlas font.
    const ImFont* font = ImGui::GetFont();
    if (!font)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if (atlas && atlas->Fonts.Size > 0)
            font = atlas->Fonts[0];
    }
    const std::string out = ansl::sort::by_brightness_utf8(s, len, font, ascending);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
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

// -------- font (textmode_font: FIGlet/TDF) --------
static textmode_font::Registry* LuaGetFontRegistry(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "phosphor.textmode_font_registry");
    auto* reg = static_cast<textmode_font::Registry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return reg;
}

static int Color32ToActivePaletteIndexOrMinus1(lua_State* L, std::uint32_t c32)
{
    if (c32 == 0)
        return -1;
    const phos::color::PaletteInstanceId pal = LuaGetActivePaletteId(L);
    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    std::uint8_t r = 0, g = 0, b = 0;
    if (!phos::color::ColorOps::UnpackImGuiAbgr(c32, r, g, b))
        return -1;
    return (int)QuantizeRgbToPaletteIndex_Quant3dOrExact(pal, r, g, b, qp);
}

static int l_font_list(lua_State* L)
{
    textmode_font::Registry* reg = LuaGetFontRegistry(L);
    if (!reg)
    {
        lua_newtable(L);
        return 1;
    }

    const auto& list = reg->List();
    lua_createtable(L, (int)list.size(), 0);
    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& e = list[i];
        lua_createtable(L, 0, 6);
        lua_pushlstring(L, e.id.data(), e.id.size()); lua_setfield(L, -2, "id");
        lua_pushlstring(L, e.label.data(), e.label.size()); lua_setfield(L, -2, "label");
        lua_pushlstring(L, e.meta.name.data(), e.meta.name.size()); lua_setfield(L, -2, "name");

        const char* kind = (e.meta.kind == textmode_font::Kind::Tdf) ? "tdf" : "flf";
        lua_pushstring(L, kind); lua_setfield(L, -2, "kind");

        if (e.meta.kind == textmode_font::Kind::Tdf)
        {
            const char* t = "block";
            if (e.meta.tdf_type == textmode_font::TdfFontType::Outline) t = "outline";
            else if (e.meta.tdf_type == textmode_font::TdfFontType::Color) t = "color";
            lua_pushstring(L, t); lua_setfield(L, -2, "tdfType");
            lua_pushinteger(L, e.meta.spacing); lua_setfield(L, -2, "spacing");
        }

        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int l_font_errors(lua_State* L)
{
    textmode_font::Registry* reg = LuaGetFontRegistry(L);
    if (!reg)
    {
        lua_newtable(L);
        return 1;
    }
    const auto& errs = reg->Errors();
    lua_createtable(L, (int)errs.size(), 0);
    for (size_t i = 0; i < errs.size(); ++i)
    {
        lua_pushlstring(L, errs[i].data(), errs[i].size());
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int l_font_render(lua_State* L)
{
    textmode_font::Registry* reg = LuaGetFontRegistry(L);
    if (!reg)
    {
        lua_pushnil(L);
        lua_pushliteral(L, "font registry not initialized");
        return 2;
    }

    size_t id_len = 0;
    const char* id = luaL_checklstring(L, 1, &id_len);
    size_t text_len = 0;
    const char* text = luaL_optlstring(L, 2, "", &text_len);

    textmode_font::RenderOptions opts;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3))
    {
        lua_getfield(L, 3, "editMode");
        if (lua_isboolean(L, -1))
            opts.mode = lua_toboolean(L, -1) ? textmode_font::RenderMode::Edit : textmode_font::RenderMode::Display;
        lua_pop(L, 1);

        lua_getfield(L, 3, "outlineStyle");
        if (lua_isnumber(L, -1))
            opts.outline_style = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "useFontColors");
        if (lua_isboolean(L, -1))
            opts.use_font_colors = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 3, "icecolors");
        if (lua_isboolean(L, -1))
            opts.icecolors = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    textmode_font::Bitmap bmp;
    std::string err;
    if (!reg->Render(std::string_view(id, id_len), std::string_view(text, text_len), opts, bmp, err))
    {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }

    const int w = bmp.w;
    const int h = bmp.h;
    const int n = (w > 0 && h > 0) ? (w * h) : 0;

    lua_createtable(L, 0, 5);
    lua_pushinteger(L, w); lua_setfield(L, -2, "w");
    lua_pushinteger(L, h); lua_setfield(L, -2, "h");

    lua_createtable(L, n, 0); // cp
    lua_createtable(L, n, 0); // fg
    lua_createtable(L, n, 0); // bg
    for (int i = 0; i < n; ++i)
    {
        const char32_t cp = (i < (int)bmp.cp.size()) ? bmp.cp[(size_t)i] : U' ';
        const std::uint32_t fg32 = (i < (int)bmp.fg.size()) ? bmp.fg[(size_t)i] : 0u;
        const std::uint32_t bg32 = (i < (int)bmp.bg.size()) ? bmp.bg[(size_t)i] : 0u;

        lua_pushinteger(L, (lua_Integer)cp);
        lua_rawseti(L, -4, (lua_Integer)i + 1);

        lua_pushinteger(L, (lua_Integer)Color32ToActivePaletteIndexOrMinus1(L, fg32));
        lua_rawseti(L, -3, (lua_Integer)i + 1);

        lua_pushinteger(L, (lua_Integer)Color32ToActivePaletteIndexOrMinus1(L, bg32));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    // Stack: result, cp, fg, bg
    lua_setfield(L, -4, "bg"); // result.bg = bg
    lua_setfield(L, -3, "fg"); // result.fg = fg
    lua_setfield(L, -2, "cp"); // result.cp = cp
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
        {"mod_glsl", l_num_mod_glsl},
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
        {"lengthSq", l_vec3_lengthSq},
        {"norm", l_vec3_norm},
        {"abs", l_vec3_abs},
        {"max", l_vec3_max},
        {"min", l_vec3_min},
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
        {"opUnion", l_sdf_opUnion},
        {"opIntersection", l_sdf_opIntersection},
        {"opDifference", l_sdf_opDifference},

        // hg primitives (2D + 3D)
        {"fSphere", l_sdf_fSphere},
        {"fPlane", l_sdf_fPlane},
        {"fBoxCheap", l_sdf_fBoxCheap},
        {"fBox", l_sdf_fBox},
        {"fBox2Cheap", l_sdf_fBox2Cheap},
        {"fBox2", l_sdf_fBox2},
        {"fCorner", l_sdf_fCorner},
        {"fBlob", l_sdf_fBlob},
        {"fCylinder", l_sdf_fCylinder},
        {"fCapsule", l_sdf_fCapsule},
        {"fLineSegment", l_sdf_fLineSegment},
        {"fTorus", l_sdf_fTorus},
        {"fCircle", l_sdf_fCircle},
        {"fDisc", l_sdf_fDisc},
        {"fHexagonCircumcircle", l_sdf_fHexagonCircumcircle},
        {"fHexagonIncircle", l_sdf_fHexagonIncircle},
        {"fCone", l_sdf_fCone},
        {"fGDF", l_sdf_fGDF},
        {"fOctahedron", l_sdf_fOctahedron},
        {"fDodecahedron", l_sdf_fDodecahedron},
        {"fIcosahedron", l_sdf_fIcosahedron},
        {"fTruncatedOctahedron", l_sdf_fTruncatedOctahedron},
        {"fTruncatedIcosahedron", l_sdf_fTruncatedIcosahedron},

        // hg domain ops (inout-style => return (p', cell/sign))
        {"pR", l_sdf_pR},
        {"pR45", l_sdf_pR45},
        {"pMod1", l_sdf_pMod1},
        {"pModMirror1", l_sdf_pModMirror1},
        {"pModSingle1", l_sdf_pModSingle1},
        {"pModInterval1", l_sdf_pModInterval1},
        {"pModPolar", l_sdf_pModPolar},
        {"pMod2", l_sdf_pMod2},
        {"pModMirror2", l_sdf_pModMirror2},
        {"pModGrid2", l_sdf_pModGrid2},
        {"pMod3", l_sdf_pMod3},
        {"pMirror", l_sdf_pMirror},
        {"pMirrorOctant", l_sdf_pMirrorOctant},
        {"pReflect", l_sdf_pReflect},

        // hg object combination operators
        {"fOpUnionChamfer", l_sdf_fOpUnionChamfer},
        {"fOpIntersectionChamfer", l_sdf_fOpIntersectionChamfer},
        {"fOpDifferenceChamfer", l_sdf_fOpDifferenceChamfer},
        {"fOpUnionRound", l_sdf_fOpUnionRound},
        {"fOpIntersectionRound", l_sdf_fOpIntersectionRound},
        {"fOpDifferenceRound", l_sdf_fOpDifferenceRound},
        {"fOpUnionColumns", l_sdf_fOpUnionColumns},
        {"fOpDifferenceColumns", l_sdf_fOpDifferenceColumns},
        {"fOpIntersectionColumns", l_sdf_fOpIntersectionColumns},
        {"fOpUnionStairs", l_sdf_fOpUnionStairs},
        {"fOpIntersectionStairs", l_sdf_fOpIntersectionStairs},
        {"fOpDifferenceStairs", l_sdf_fOpDifferenceStairs},
        {"fOpUnionSoft", l_sdf_fOpUnionSoft},
        {"fOpPipe", l_sdf_fOpPipe},
        {"fOpEngrave", l_sdf_fOpEngrave},
        {"fOpGroove", l_sdf_fOpGroove},
        {"fOpTongue", l_sdf_fOpTongue},
        {nullptr, nullptr},
    };
    SetFuncs(L, sdf_fns);
    lua_setfield(L, -2, "sdf");

    // color (active palette indices)
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

    // ANSI16/VGA16 named colors. These are mapped into the active palette on access (palette-aware).
    lua_newtable(L); // ansi16
    lua_newtable(L); // mt
    lua_pushcfunction(L, l_color_ansi16_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2); // setmetatable(ansi16, mt)
    lua_setfield(L, -2, "ansi16");

    lua_setfield(L, -2, "color");

    // buffer (portable)
    lua_createtable(L, 0, 0);
    SetFuncs(L, buffer_fns);
    lua_setfield(L, -2, "buffer");

    // font (FIGlet / TheDraw text-art fonts)
    lua_createtable(L, 0, 0);
    static const luaL_Reg font_fns[] = {
        {"list", l_font_list},
        {"render", l_font_render},   // (id, text, opts?) -> {w,h,cp[],fg[],bg[]} | (nil, err)
        {"errors", l_font_errors},   // () -> { "error...", ... }
        {nullptr, nullptr},
    };
    SetFuncs(L, font_fns);
    lua_setfield(L, -2, "font");

    // sort (host: uses ImGui font atlas to sort glyphs by brightness)
    lua_createtable(L, 0, 0);
    static const luaL_Reg sort_fns[] = {
        {"brightness", l_sort_brightness}, // (utf8, ascending?) -> utf8
        {nullptr, nullptr},
    };
    SetFuncs(L, sort_fns);
    lua_setfield(L, -2, "sort");
    // drawbox: host-specific (depends on styling + higher-level layout); keep as stub for now
    lua_createtable(L, 0, 0);
    lua_setfield(L, -2, "drawbox");
    // string (minimal, plus UTF-8 helpers for LuaJIT)
    lua_createtable(L, 0, 0);
    SetFuncs(L, string_fns);
    lua_setfield(L, -2, "string");

    // noise (libnoise)
    noise_lua::PushNoiseModule(L);
    lua_setfield(L, -2, "noise");

    return 1;
}


