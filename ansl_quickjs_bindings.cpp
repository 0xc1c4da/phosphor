#include "ansl_quickjs_bindings.h"

#include "ansl_native.h"

#if __has_include(<quickjs/quickjs.h>)
#include <quickjs/quickjs.h>
#else
#include <quickjs.h>
#endif

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>

namespace
{
static JSValue ThrowTypeError(JSContext* ctx, const char* msg)
{
    return JS_ThrowTypeError(ctx, "%s", msg ? msg : "TypeError");
}

static JSValue ThrowError(JSContext* ctx, const char* msg)
{
    return JS_ThrowInternalError(ctx, "%s", msg ? msg : "Error");
}

static bool JsGetNumber(JSContext* ctx, JSValueConst v, double& out)
{
    if (JS_IsNumber(v))
        return JS_ToFloat64(ctx, &out, v) == 0;
    if (JS_IsBool(v))
    {
        out = JS_ToBool(ctx, v) ? 1.0 : 0.0;
        return true;
    }
    return false;
}

static bool JsGetInt32(JSContext* ctx, JSValueConst v, int32_t& out)
{
    return JS_ToInt32(ctx, &out, v) == 0;
}

static bool JsGetPropNumber(JSContext* ctx, JSValueConst obj, const char* key, double& out)
{
    JSValue pv = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(pv))
        return false;
    const bool ok = JsGetNumber(ctx, pv, out);
    JS_FreeValue(ctx, pv);
    return ok;
}

static bool JsReadVec2(JSContext* ctx, JSValueConst v, ansl::Vec2& out)
{
    if (!JS_IsObject(v))
        return false;
    double x = 0.0, y = 0.0;
    if (!JsGetPropNumber(ctx, v, "x", x) || !JsGetPropNumber(ctx, v, "y", y))
        return false;
    out.x = x;
    out.y = y;
    return true;
}

static bool JsReadVec3(JSContext* ctx, JSValueConst v, ansl::Vec3& out)
{
    if (!JS_IsObject(v))
        return false;
    double x = 0.0, y = 0.0, z = 0.0;
    if (!JsGetPropNumber(ctx, v, "x", x) || !JsGetPropNumber(ctx, v, "y", y) || !JsGetPropNumber(ctx, v, "z", z))
        return false;
    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

static JSValue JsWriteVec2(JSContext* ctx, const ansl::Vec2& v, JSValueConst out_opt)
{
    JSValue obj = JS_UNDEFINED;
    if (JS_IsObject(out_opt))
        obj = JS_DupValue(ctx, out_opt);
    else
        obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
    return obj;
}

static JSValue JsWriteVec3(JSContext* ctx, const ansl::Vec3& v, JSValueConst out_opt)
{
    JSValue obj = JS_UNDEFINED;
    if (JS_IsObject(out_opt))
        obj = JS_DupValue(ctx, out_opt);
    else
        obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.z));
    return obj;
}

// -------- num --------
static JSValue Js_num_map(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 5) return ThrowTypeError(ctx, "num.map(v,inA,inB,outA,outB) expects 5 args");
    double v, inA, inB, outA, outB;
    if (!JsGetNumber(ctx, argv[0], v) || !JsGetNumber(ctx, argv[1], inA) || !JsGetNumber(ctx, argv[2], inB) ||
        !JsGetNumber(ctx, argv[3], outA) || !JsGetNumber(ctx, argv[4], outB))
        return ThrowTypeError(ctx, "num.map expects numbers");
    return JS_NewFloat64(ctx, ansl::num::map(v, inA, inB, outA, outB));
}

static JSValue Js_num_fract(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "num.fract(v) expects 1 arg");
    double v;
    if (!JsGetNumber(ctx, argv[0], v)) return ThrowTypeError(ctx, "num.fract expects a number");
    return JS_NewFloat64(ctx, ansl::num::fract(v));
}

static JSValue Js_num_clamp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "num.clamp(v,min,max) expects 3 args");
    double v, mn, mx;
    if (!JsGetNumber(ctx, argv[0], v) || !JsGetNumber(ctx, argv[1], mn) || !JsGetNumber(ctx, argv[2], mx))
        return ThrowTypeError(ctx, "num.clamp expects numbers");
    return JS_NewFloat64(ctx, ansl::num::clamp(v, mn, mx));
}

static JSValue Js_num_sign(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "num.sign(n) expects 1 arg");
    double n;
    if (!JsGetNumber(ctx, argv[0], n)) return ThrowTypeError(ctx, "num.sign expects a number");
    return JS_NewFloat64(ctx, ansl::num::sign(n));
}

static JSValue Js_num_mix(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "num.mix(v1,v2,a) expects 3 args");
    double v1, v2, a;
    if (!JsGetNumber(ctx, argv[0], v1) || !JsGetNumber(ctx, argv[1], v2) || !JsGetNumber(ctx, argv[2], a))
        return ThrowTypeError(ctx, "num.mix expects numbers");
    return JS_NewFloat64(ctx, ansl::num::mix(v1, v2, a));
}

static JSValue Js_num_step(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "num.step(edge,x) expects 2 args");
    double edge, x;
    if (!JsGetNumber(ctx, argv[0], edge) || !JsGetNumber(ctx, argv[1], x))
        return ThrowTypeError(ctx, "num.step expects numbers");
    return JS_NewFloat64(ctx, ansl::num::step(edge, x));
}

static JSValue Js_num_smoothstep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "num.smoothstep(edge0,edge1,t) expects 3 args");
    double e0, e1, t;
    if (!JsGetNumber(ctx, argv[0], e0) || !JsGetNumber(ctx, argv[1], e1) || !JsGetNumber(ctx, argv[2], t))
        return ThrowTypeError(ctx, "num.smoothstep expects numbers");
    return JS_NewFloat64(ctx, ansl::num::smoothstep(e0, e1, t));
}

static JSValue Js_num_smootherstep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "num.smootherstep(edge0,edge1,t) expects 3 args");
    double e0, e1, t;
    if (!JsGetNumber(ctx, argv[0], e0) || !JsGetNumber(ctx, argv[1], e1) || !JsGetNumber(ctx, argv[2], t))
        return ThrowTypeError(ctx, "num.smootherstep expects numbers");
    return JS_NewFloat64(ctx, ansl::num::smootherstep(e0, e1, t));
}

static JSValue Js_num_mod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "num.mod(a,b) expects 2 args");
    double a, b;
    if (!JsGetNumber(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], b))
        return ThrowTypeError(ctx, "num.mod expects numbers");
    return JS_NewFloat64(ctx, ansl::num::mod(a, b));
}

// -------- vec2 --------
static JSValue Js_vec2_vec2(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.vec2(x,y) expects 2 args");
    double x, y;
    if (!JsGetNumber(ctx, argv[0], x) || !JsGetNumber(ctx, argv[1], y))
        return ThrowTypeError(ctx, "vec2.vec2 expects numbers");
    return JsWriteVec2(ctx, ansl::Vec2{x, y}, JS_UNDEFINED);
}

static JSValue Js_vec2_copy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.copy(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.copy expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, a, out_opt);
}

static JSValue Js_vec2_add(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.add(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.add expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::add(a, b), out_opt);
}

static JSValue Js_vec2_sub(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.sub(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.sub expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::sub(a, b), out_opt);
}

static JSValue Js_vec2_mulN(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.mulN(a,k,out?) expects at least 2 args");
    ansl::Vec2 a;
    double k;
    if (!JsReadVec2(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], k)) return ThrowTypeError(ctx, "vec2.mulN expects ({x,y}, number)");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::mulN(a, k), out_opt);
}

static JSValue Js_vec2_mul(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.mul(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.mul expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::mul(a, b), out_opt);
}

static JSValue Js_vec2_div(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.div(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.div expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::div(a, b), out_opt);
}

static JSValue Js_vec2_addN(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.addN(a,k,out?) expects at least 2 args");
    ansl::Vec2 a;
    double k;
    if (!JsReadVec2(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], k)) return ThrowTypeError(ctx, "vec2.addN expects ({x,y}, number)");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::addN(a, k), out_opt);
}

static JSValue Js_vec2_subN(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.subN(a,k,out?) expects at least 2 args");
    ansl::Vec2 a;
    double k;
    if (!JsReadVec2(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], k)) return ThrowTypeError(ctx, "vec2.subN expects ({x,y}, number)");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::subN(a, k), out_opt);
}

static JSValue Js_vec2_divN(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.divN(a,k,out?) expects at least 2 args");
    ansl::Vec2 a;
    double k;
    if (!JsReadVec2(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], k)) return ThrowTypeError(ctx, "vec2.divN expects ({x,y}, number)");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::divN(a, k), out_opt);
}

static JSValue Js_vec2_dot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.dot(a,b) expects 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.dot expects {x,y}");
    return JS_NewFloat64(ctx, ansl::vec2::dot(a, b));
}

static JSValue Js_vec2_length(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.length(a) expects 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.length expects {x,y}");
    return JS_NewFloat64(ctx, ansl::vec2::length(a));
}

static JSValue Js_vec2_lengthSq(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.lengthSq(a) expects 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.lengthSq expects {x,y}");
    return JS_NewFloat64(ctx, ansl::vec2::lengthSq(a));
}

static JSValue Js_vec2_dist(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.dist(a,b) expects 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.dist expects {x,y}");
    return JS_NewFloat64(ctx, ansl::vec2::dist(a, b));
}

static JSValue Js_vec2_distSq(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.distSq(a,b) expects 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.distSq expects {x,y}");
    return JS_NewFloat64(ctx, ansl::vec2::distSq(a, b));
}

static JSValue Js_vec2_norm(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.norm(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.norm expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::norm(a), out_opt);
}

static JSValue Js_vec2_neg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.neg(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.neg expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::neg(a), out_opt);
}

static JSValue Js_vec2_rot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.rot(a,ang,out?) expects at least 2 args");
    ansl::Vec2 a;
    double ang;
    if (!JsReadVec2(ctx, argv[0], a) || !JsGetNumber(ctx, argv[1], ang)) return ThrowTypeError(ctx, "vec2.rot expects ({x,y}, number)");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::rot(a, ang), out_opt);
}

static JSValue Js_vec2_mix(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "vec2.mix(a,b,t,out?) expects at least 3 args");
    ansl::Vec2 a, b;
    double t;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b) || !JsGetNumber(ctx, argv[2], t))
        return ThrowTypeError(ctx, "vec2.mix expects ({x,y},{x,y}, number)");
    JSValueConst out_opt = (argc >= 4) ? argv[3] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::mix(a, b, t), out_opt);
}

static JSValue Js_vec2_abs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.abs(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.abs expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::abs(a), out_opt);
}

static JSValue Js_vec2_max(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.max(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.max expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::max(a, b), out_opt);
}

static JSValue Js_vec2_min(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec2.min(a,b,out?) expects at least 2 args");
    ansl::Vec2 a, b;
    if (!JsReadVec2(ctx, argv[0], a) || !JsReadVec2(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec2.min expects {x,y}");
    JSValueConst out_opt = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::min(a, b), out_opt);
}

static JSValue Js_vec2_fract(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.fract(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.fract expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::fract(a), out_opt);
}

static JSValue Js_vec2_floor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.floor(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.floor expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::floor(a), out_opt);
}

static JSValue Js_vec2_ceil(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.ceil(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.ceil expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::ceil(a), out_opt);
}

static JSValue Js_vec2_round(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec2.round(a,out?) expects at least 1 arg");
    ansl::Vec2 a;
    if (!JsReadVec2(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec2.round expects {x,y}");
    JSValueConst out_opt = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    return JsWriteVec2(ctx, ansl::vec2::round(a), out_opt);
}

// -------- vec3 (minimal subset) --------
static JSValue Js_vec3_vec3(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "vec3.vec3(x,y,z) expects 3 args");
    double x, y, z;
    if (!JsGetNumber(ctx, argv[0], x) || !JsGetNumber(ctx, argv[1], y) || !JsGetNumber(ctx, argv[2], z))
        return ThrowTypeError(ctx, "vec3.vec3 expects numbers");
    return JsWriteVec3(ctx, ansl::Vec3{x, y, z}, JS_UNDEFINED);
}

static JSValue Js_vec3_dot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "vec3.dot(a,b) expects 2 args");
    ansl::Vec3 a, b;
    if (!JsReadVec3(ctx, argv[0], a) || !JsReadVec3(ctx, argv[1], b)) return ThrowTypeError(ctx, "vec3.dot expects {x,y,z}");
    return JS_NewFloat64(ctx, ansl::vec3::dot(a, b));
}

static JSValue Js_vec3_length(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "vec3.length(a) expects 1 arg");
    ansl::Vec3 a;
    if (!JsReadVec3(ctx, argv[0], a)) return ThrowTypeError(ctx, "vec3.length expects {x,y,z}");
    return JS_NewFloat64(ctx, ansl::vec3::length(a));
}

// -------- sdf --------
static JSValue Js_sdf_sdCircle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "sdf.sdCircle(p,radius) expects 2 args");
    ansl::Vec2 p;
    double r;
    if (!JsReadVec2(ctx, argv[0], p) || !JsGetNumber(ctx, argv[1], r)) return ThrowTypeError(ctx, "sdf.sdCircle expects ({x,y}, number)");
    return JS_NewFloat64(ctx, ansl::sdf::sdCircle(p, r));
}

static JSValue Js_sdf_sdBox(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return ThrowTypeError(ctx, "sdf.sdBox(p,size) expects 2 args");
    ansl::Vec2 p, size;
    if (!JsReadVec2(ctx, argv[0], p) || !JsReadVec2(ctx, argv[1], size)) return ThrowTypeError(ctx, "sdf.sdBox expects ({x,y}, {x,y})");
    return JS_NewFloat64(ctx, ansl::sdf::sdBox(p, size));
}

static JSValue Js_sdf_opSmoothUnion(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "sdf.opSmoothUnion(d1,d2,k) expects 3 args");
    double d1, d2, k;
    if (!JsGetNumber(ctx, argv[0], d1) || !JsGetNumber(ctx, argv[1], d2) || !JsGetNumber(ctx, argv[2], k))
        return ThrowTypeError(ctx, "sdf.opSmoothUnion expects numbers");
    return JS_NewFloat64(ctx, ansl::sdf::opSmoothUnion(d1, d2, k));
}

static JSValue Js_sdf_opSmoothSubtraction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "sdf.opSmoothSubtraction(d1,d2,k) expects 3 args");
    double d1, d2, k;
    if (!JsGetNumber(ctx, argv[0], d1) || !JsGetNumber(ctx, argv[1], d2) || !JsGetNumber(ctx, argv[2], k))
        return ThrowTypeError(ctx, "sdf.opSmoothSubtraction expects numbers");
    return JS_NewFloat64(ctx, ansl::sdf::opSmoothSubtraction(d1, d2, k));
}

static JSValue Js_sdf_opSmoothIntersection(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "sdf.opSmoothIntersection(d1,d2,k) expects 3 args");
    double d1, d2, k;
    if (!JsGetNumber(ctx, argv[0], d1) || !JsGetNumber(ctx, argv[1], d2) || !JsGetNumber(ctx, argv[2], k))
        return ThrowTypeError(ctx, "sdf.opSmoothIntersection expects numbers");
    return JS_NewFloat64(ctx, ansl::sdf::opSmoothIntersection(d1, d2, k));
}

// -------- color (minimal helpers only; palettes not ported yet) --------
static JSValue Js_color_rgb(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return ThrowTypeError(ctx, "color.rgb(r,g,b,a?) expects at least 3 args");
    double r, g, b;
    if (!JsGetNumber(ctx, argv[0], r) || !JsGetNumber(ctx, argv[1], g) || !JsGetNumber(ctx, argv[2], b))
        return ThrowTypeError(ctx, "color.rgb expects numbers");
    double a = 1.0;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && !JsGetNumber(ctx, argv[3], a))
        return ThrowTypeError(ctx, "color.rgb alpha must be a number");
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "r", JS_NewFloat64(ctx, r));
    JS_SetPropertyStr(ctx, obj, "g", JS_NewFloat64(ctx, g));
    JS_SetPropertyStr(ctx, obj, "b", JS_NewFloat64(ctx, b));
    JS_SetPropertyStr(ctx, obj, "a", JS_NewFloat64(ctx, a));
    return obj;
}

static std::string HexByte(int v)
{
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.push_back(hexd[(v >> 4) & 0xF]);
    out.push_back(hexd[v & 0xF]);
    return out;
}

static JSValue Js_color_rgb2hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "color.rgb2hex({r,g,b,a?}) expects 1 arg");
    if (!JS_IsObject(argv[0])) return ThrowTypeError(ctx, "color.rgb2hex expects an object");
    double r = 0, g = 0, b = 0, a = 1.0;
    if (!JsGetPropNumber(ctx, argv[0], "r", r) || !JsGetPropNumber(ctx, argv[0], "g", g) || !JsGetPropNumber(ctx, argv[0], "b", b))
        return ThrowTypeError(ctx, "color.rgb2hex expects {r,g,b}");
    bool has_a = false;
    {
        JSValue av = JS_GetPropertyStr(ctx, argv[0], "a");
        if (!JS_IsUndefined(av))
        {
            has_a = true;
            if (!JsGetNumber(ctx, av, a))
            {
                JS_FreeValue(ctx, av);
                return ThrowTypeError(ctx, "color.rgb2hex expects numeric a");
            }
        }
        JS_FreeValue(ctx, av);
    }

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
    return JS_NewStringLen(ctx, out.c_str(), out.size());
}

static JSValue Js_color_rgb2css(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "color.rgb2css({r,g,b,a?}) expects 1 arg");
    if (!JS_IsObject(argv[0])) return ThrowTypeError(ctx, "color.rgb2css expects an object");
    double r = 0, g = 0, b = 0, a = 1.0;
    if (!JsGetPropNumber(ctx, argv[0], "r", r) || !JsGetPropNumber(ctx, argv[0], "g", g) || !JsGetPropNumber(ctx, argv[0], "b", b))
        return ThrowTypeError(ctx, "color.rgb2css expects {r,g,b}");

    bool has_a = false;
    {
        JSValue av = JS_GetPropertyStr(ctx, argv[0], "a");
        if (!JS_IsUndefined(av))
        {
            has_a = true;
            if (!JsGetNumber(ctx, av, a))
            {
                JS_FreeValue(ctx, av);
                return ThrowTypeError(ctx, "color.rgb2css expects numeric a");
            }
        }
        JS_FreeValue(ctx, av);
    }

    const int ri = (int)std::llround(r);
    const int gi = (int)std::llround(g);
    const int bi = (int)std::llround(b);
    std::string out;
    if (!has_a || a == 1.0)
    {
        out = "rgb(" + std::to_string(ri) + "," + std::to_string(gi) + "," + std::to_string(bi) + ")";
    }
    else
    {
        out = "rgba(" + std::to_string(ri) + "," + std::to_string(gi) + "," + std::to_string(bi) + "," + std::to_string(a) + ")";
    }
    return JS_NewStringLen(ctx, out.c_str(), out.size());
}

static JSValue Js_color_int2rgb(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return ThrowTypeError(ctx, "color.int2rgb(int) expects 1 arg");
    int32_t v = 0;
    if (!JsGetInt32(ctx, argv[0], v)) return ThrowTypeError(ctx, "color.int2rgb expects an int");
    const int32_t r = (v >> 16) & 0xff;
    const int32_t g = (v >> 8) & 0xff;
    const int32_t b = (v)&0xff;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "a", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, obj, "r", JS_NewInt32(ctx, r));
    JS_SetPropertyStr(ctx, obj, "g", JS_NewInt32(ctx, g));
    JS_SetPropertyStr(ctx, obj, "b", JS_NewInt32(ctx, b));
    return obj;
}

static JSValue Js_color_hex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    // Matches JS module helper: hex(r,g,b,a?) == rgb2hex(rgb(r,g,b,a?))
    JSValue rgb = Js_color_rgb(ctx, this_val, argc, argv);
    if (JS_IsException(rgb))
        return rgb;
    JSValue args[1] = { rgb };
    JSValue out = Js_color_rgb2hex(ctx, this_val, 1, args);
    JS_FreeValue(ctx, rgb);
    return out;
}

// -------- ANSL.runLayer(program, ctx, layer) --------
static JSValue Js_ansl_runLayer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3)
        return ThrowTypeError(ctx, "ANSL.runLayer(program, ctx, layer) expects 3 args");

    JSValueConst program = argv[0];
    JSValueConst ctx_obj = argv[1];
    JSValueConst layer_obj = argv[2];

    JSValue args[2] = { JS_DupValue(ctx, ctx_obj), JS_DupValue(ctx, layer_obj) };

    // program(ctx, layer)
    if (JS_IsFunction(ctx, program))
    {
        JSValue ret = JS_Call(ctx, program, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        return ret;
    }

    // program.render(ctx, layer)
    if (JS_IsObject(program))
    {
        JSValue render = JS_GetPropertyStr(ctx, program, "render");
        if (JS_IsFunction(ctx, render))
        {
            JSValue ret = JS_Call(ctx, render, program, 2, args);
            JS_FreeValue(ctx, render);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            return ret;
        }
        JS_FreeValue(ctx, render);
    }

    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return ThrowError(ctx, "ANSL.runLayer: program must be a function or {render()}");
}
} // namespace

bool RegisterAnslNativeQuickJS(JSContext* ctx, std::string& error)
{
    if (!ctx)
    {
        error = "RegisterAnslNativeQuickJS: ctx is null";
        return false;
    }

    JSValue global = JS_GetGlobalObject(ctx);

    // ANSL object
    JSValue ansl = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ansl, "version", JS_NewString(ctx, "1.1"));

    // modules object
    JSValue modules = JS_NewObject(ctx);

    // num
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "map", JS_NewCFunction(ctx, Js_num_map, "map", 5));
        JS_SetPropertyStr(ctx, m, "fract", JS_NewCFunction(ctx, Js_num_fract, "fract", 1));
        JS_SetPropertyStr(ctx, m, "clamp", JS_NewCFunction(ctx, Js_num_clamp, "clamp", 3));
        JS_SetPropertyStr(ctx, m, "sign", JS_NewCFunction(ctx, Js_num_sign, "sign", 1));
        JS_SetPropertyStr(ctx, m, "mix", JS_NewCFunction(ctx, Js_num_mix, "mix", 3));
        JS_SetPropertyStr(ctx, m, "step", JS_NewCFunction(ctx, Js_num_step, "step", 2));
        JS_SetPropertyStr(ctx, m, "smoothstep", JS_NewCFunction(ctx, Js_num_smoothstep, "smoothstep", 3));
        JS_SetPropertyStr(ctx, m, "smootherstep", JS_NewCFunction(ctx, Js_num_smootherstep, "smootherstep", 3));
        JS_SetPropertyStr(ctx, m, "mod", JS_NewCFunction(ctx, Js_num_mod, "mod", 2));
        JS_SetPropertyStr(ctx, modules, "num", m);
    }

    // vec2 (subset needed by sdf + common programs)
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "vec2", JS_NewCFunction(ctx, Js_vec2_vec2, "vec2", 2));
        JS_SetPropertyStr(ctx, m, "copy", JS_NewCFunction(ctx, Js_vec2_copy, "copy", 1));
        JS_SetPropertyStr(ctx, m, "add", JS_NewCFunction(ctx, Js_vec2_add, "add", 2));
        JS_SetPropertyStr(ctx, m, "sub", JS_NewCFunction(ctx, Js_vec2_sub, "sub", 2));
        JS_SetPropertyStr(ctx, m, "mul", JS_NewCFunction(ctx, Js_vec2_mul, "mul", 2));
        JS_SetPropertyStr(ctx, m, "div", JS_NewCFunction(ctx, Js_vec2_div, "div", 2));
        JS_SetPropertyStr(ctx, m, "addN", JS_NewCFunction(ctx, Js_vec2_addN, "addN", 2));
        JS_SetPropertyStr(ctx, m, "subN", JS_NewCFunction(ctx, Js_vec2_subN, "subN", 2));
        JS_SetPropertyStr(ctx, m, "mulN", JS_NewCFunction(ctx, Js_vec2_mulN, "mulN", 2));
        JS_SetPropertyStr(ctx, m, "divN", JS_NewCFunction(ctx, Js_vec2_divN, "divN", 2));
        JS_SetPropertyStr(ctx, m, "dot", JS_NewCFunction(ctx, Js_vec2_dot, "dot", 2));
        JS_SetPropertyStr(ctx, m, "length", JS_NewCFunction(ctx, Js_vec2_length, "length", 1));
        JS_SetPropertyStr(ctx, m, "lengthSq", JS_NewCFunction(ctx, Js_vec2_lengthSq, "lengthSq", 1));
        JS_SetPropertyStr(ctx, m, "dist", JS_NewCFunction(ctx, Js_vec2_dist, "dist", 2));
        JS_SetPropertyStr(ctx, m, "distSq", JS_NewCFunction(ctx, Js_vec2_distSq, "distSq", 2));
        JS_SetPropertyStr(ctx, m, "norm", JS_NewCFunction(ctx, Js_vec2_norm, "norm", 1));
        JS_SetPropertyStr(ctx, m, "neg", JS_NewCFunction(ctx, Js_vec2_neg, "neg", 1));
        JS_SetPropertyStr(ctx, m, "rot", JS_NewCFunction(ctx, Js_vec2_rot, "rot", 2));
        JS_SetPropertyStr(ctx, m, "mix", JS_NewCFunction(ctx, Js_vec2_mix, "mix", 3));
        JS_SetPropertyStr(ctx, m, "abs", JS_NewCFunction(ctx, Js_vec2_abs, "abs", 1));
        JS_SetPropertyStr(ctx, m, "max", JS_NewCFunction(ctx, Js_vec2_max, "max", 2));
        JS_SetPropertyStr(ctx, m, "min", JS_NewCFunction(ctx, Js_vec2_min, "min", 2));
        JS_SetPropertyStr(ctx, m, "fract", JS_NewCFunction(ctx, Js_vec2_fract, "fract", 1));
        JS_SetPropertyStr(ctx, m, "floor", JS_NewCFunction(ctx, Js_vec2_floor, "floor", 1));
        JS_SetPropertyStr(ctx, m, "ceil", JS_NewCFunction(ctx, Js_vec2_ceil, "ceil", 1));
        JS_SetPropertyStr(ctx, m, "round", JS_NewCFunction(ctx, Js_vec2_round, "round", 1));
        JS_SetPropertyStr(ctx, modules, "vec2", m);
    }

    // vec3 (minimal)
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "vec3", JS_NewCFunction(ctx, Js_vec3_vec3, "vec3", 3));
        JS_SetPropertyStr(ctx, m, "dot", JS_NewCFunction(ctx, Js_vec3_dot, "dot", 2));
        JS_SetPropertyStr(ctx, m, "length", JS_NewCFunction(ctx, Js_vec3_length, "length", 1));
        JS_SetPropertyStr(ctx, modules, "vec3", m);
    }

    // sdf
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "sdCircle", JS_NewCFunction(ctx, Js_sdf_sdCircle, "sdCircle", 2));
        JS_SetPropertyStr(ctx, m, "sdBox", JS_NewCFunction(ctx, Js_sdf_sdBox, "sdBox", 2));
        JS_SetPropertyStr(ctx, m, "opSmoothUnion", JS_NewCFunction(ctx, Js_sdf_opSmoothUnion, "opSmoothUnion", 3));
        JS_SetPropertyStr(ctx, m, "opSmoothSubtraction", JS_NewCFunction(ctx, Js_sdf_opSmoothSubtraction, "opSmoothSubtraction", 3));
        JS_SetPropertyStr(ctx, m, "opSmoothIntersection", JS_NewCFunction(ctx, Js_sdf_opSmoothIntersection, "opSmoothIntersection", 3));
        JS_SetPropertyStr(ctx, modules, "sdf", m);
    }

    // color (minimal)
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "rgb", JS_NewCFunction(ctx, Js_color_rgb, "rgb", 3));
        JS_SetPropertyStr(ctx, m, "hex", JS_NewCFunction(ctx, Js_color_hex, "hex", 3));
        JS_SetPropertyStr(ctx, m, "rgb2hex", JS_NewCFunction(ctx, Js_color_rgb2hex, "rgb2hex", 1));
        JS_SetPropertyStr(ctx, m, "rgb2css", JS_NewCFunction(ctx, Js_color_rgb2css, "rgb2css", 1));
        JS_SetPropertyStr(ctx, m, "int2rgb", JS_NewCFunction(ctx, Js_color_int2rgb, "int2rgb", 1));
        // Palettes intentionally not ported yet; provide empty containers.
        JS_SetPropertyStr(ctx, m, "CSS1", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, m, "CSS2", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, m, "CSS3", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, m, "CSS4", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, m, "C64", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, m, "CGA", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, modules, "color", m);
    }

    // Stubs for modules that exist in ansl/src/index.js exports but are not yet ported here.
    JS_SetPropertyStr(ctx, modules, "buffer", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, modules, "drawbox", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, modules, "string", JS_NewObject(ctx));

    JS_SetPropertyStr(ctx, ansl, "modules", modules);
    JS_SetPropertyStr(ctx, ansl, "runLayer", JS_NewCFunction(ctx, Js_ansl_runLayer, "runLayer", 3));

    JS_SetPropertyStr(ctx, global, "ANSL", ansl);
    JS_FreeValue(ctx, global);

    error.clear();
    return true;
}


