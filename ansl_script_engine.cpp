#include "ansl_script_engine.h"

#include "canvas.h"

#if __has_include(<quickjs/quickjs.h>)
#include <quickjs/quickjs.h>
#else
#include <quickjs.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace
{
static bool ReadFileToString(const std::string& path, std::string& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "Failed to open: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    error.clear();
    return true;
}

static std::string JsToStdString(JSContext* ctx, JSValueConst v)
{
    const char* cstr = JS_ToCString(ctx, v);
    if (!cstr)
        return {};
    std::string out = cstr;
    JS_FreeCString(ctx, cstr);
    return out;
}

static char32_t DecodeFirstUtf8Codepoint(const char* s, size_t len)
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

static void DecodeUtf8ToCodepoints(const char* s, size_t len, std::vector<char32_t>& out)
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

static char32_t JsCharArg(JSContext* ctx, JSValueConst v)
{
    if (JS_IsNumber(v))
    {
        int32_t cp = 0;
        if (JS_ToInt32(ctx, &cp, v) == 0)
        {
            if (cp < 0) cp = 0;
            return static_cast<char32_t>(cp);
        }
    }

    size_t len = 0;
    const char* cstr = JS_ToCStringLen(ctx, &len, v);
    if (!cstr)
        return U' ';
    char32_t cp = DecodeFirstUtf8Codepoint(cstr, len);
    JS_FreeCString(ctx, cstr);
    return cp;
}

static JSValue ThrowTypeError(JSContext* ctx, const char* msg)
{
    return JS_ThrowTypeError(ctx, "%s", msg ? msg : "TypeError");
}

static JSValue ThrowError(JSContext* ctx, const char* msg)
{
    return JS_ThrowInternalError(ctx, "%s", msg ? msg : "Error");
}

struct LayerBinding
{
    AnsiCanvas* canvas = nullptr;
    int         layer_index = 0;
};

static JSClassID g_layer_class_id = 0;

static void LayerFinalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    auto* binding = static_cast<LayerBinding*>(JS_GetOpaque(val, g_layer_class_id));
    delete binding;
}

static JSValue LayerSet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* binding = static_cast<LayerBinding*>(JS_GetOpaque2(ctx, this_val, g_layer_class_id));
    if (!binding || !binding->canvas)
        return ThrowError(ctx, "Invalid layer binding");
    if (argc < 3)
        return ThrowTypeError(ctx, "layer.set(x, y, cpOrString) expects 3 args");

    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) != 0 || JS_ToInt32(ctx, &y, argv[1]) != 0)
        return ThrowTypeError(ctx, "x/y must be integers");

    char32_t cp = JsCharArg(ctx, argv[2]);
    binding->canvas->SetLayerCell(binding->layer_index, y, x, cp);
    return JS_UNDEFINED;
}

static JSValue LayerGet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* binding = static_cast<LayerBinding*>(JS_GetOpaque2(ctx, this_val, g_layer_class_id));
    if (!binding || !binding->canvas)
        return ThrowError(ctx, "Invalid layer binding");
    if (argc < 2)
        return ThrowTypeError(ctx, "layer.get(x, y) expects 2 args");

    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) != 0 || JS_ToInt32(ctx, &y, argv[1]) != 0)
        return ThrowTypeError(ctx, "x/y must be integers");

    char32_t cp = binding->canvas->GetLayerCell(binding->layer_index, y, x);

    // Encode cp to UTF-8 (minimal; enough for common BMP and beyond).
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
    return JS_NewStringLen(ctx, out, (size_t)n);
}

static JSValue LayerClear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* binding = static_cast<LayerBinding*>(JS_GetOpaque2(ctx, this_val, g_layer_class_id));
    if (!binding || !binding->canvas)
        return ThrowError(ctx, "Invalid layer binding");

    char32_t fill = U' ';
    if (argc >= 1)
        fill = JsCharArg(ctx, argv[0]);
    binding->canvas->ClearLayer(binding->layer_index, fill);
    return JS_UNDEFINED;
}

static JSValue LayerSetRow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* binding = static_cast<LayerBinding*>(JS_GetOpaque2(ctx, this_val, g_layer_class_id));
    if (!binding || !binding->canvas)
        return ThrowError(ctx, "Invalid layer binding");
    if (argc < 2)
        return ThrowTypeError(ctx, "layer.setRow(y, utf8String) expects 2 args");

    int32_t y = 0;
    if (JS_ToInt32(ctx, &y, argv[0]) != 0)
        return ThrowTypeError(ctx, "y must be an integer");
    if (y < 0) y = 0;

    size_t len = 0;
    const char* cstr = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!cstr)
        return ThrowTypeError(ctx, "row must be a string");

    std::vector<char32_t> cps;
    DecodeUtf8ToCodepoints(cstr, len, cps);
    JS_FreeCString(ctx, cstr);

    const int cols = binding->canvas->GetColumns();
    binding->canvas->EnsureRowsPublic(y + 1);
    for (int x = 0; x < cols; ++x)
    {
        char32_t cp = (x < (int)cps.size()) ? cps[(size_t)x] : U' ';
        binding->canvas->SetLayerCell(binding->layer_index, y, x, cp);
    }
    return JS_UNDEFINED;
}

static JSValue JsPrint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    for (int i = 0; i < argc; ++i)
    {
        std::string s = JsToStdString(ctx, argv[i]);
        std::fwrite(s.data(), 1, s.size(), stderr);
        if (i + 1 < argc)
            std::fwrite(" ", 1, 1, stderr);
    }
    std::fwrite("\n", 1, 1, stderr);
    return JS_UNDEFINED;
}

static void RegisterLayerClass(JSRuntime* rt)
{
    if (g_layer_class_id != 0)
        return;

    JS_NewClassID(&g_layer_class_id);
    JSClassDef def = {};
    def.class_name = "AnsiLayer";
    def.finalizer = LayerFinalizer;
    JS_NewClass(rt, g_layer_class_id, &def);
}

static JSValue NewLayerObject(JSContext* ctx, AnsiCanvas* canvas, int layer_index)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    RegisterLayerClass(rt);

    JSValue obj = JS_NewObjectClass(ctx, g_layer_class_id);
    if (JS_IsException(obj))
        return obj;

    auto* binding = new LayerBinding();
    binding->canvas = canvas;
    binding->layer_index = layer_index;
    JS_SetOpaque(obj, binding);

    // Avoid QuickJS header variants that use designated initializers in JS_CFUNC_DEF,
    // which some C++ compilers reject. Bind methods explicitly instead.
    JS_SetPropertyStr(ctx, obj, "set", JS_NewCFunction(ctx, LayerSet, "set", 3));
    JS_SetPropertyStr(ctx, obj, "get", JS_NewCFunction(ctx, LayerGet, "get", 2));
    JS_SetPropertyStr(ctx, obj, "clear", JS_NewCFunction(ctx, LayerClear, "clear", 1));
    JS_SetPropertyStr(ctx, obj, "setRow", JS_NewCFunction(ctx, LayerSetRow, "setRow", 2));
    return obj;
}

static std::string FormatException(JSContext* ctx)
{
    JSValue ex = JS_GetException(ctx);
    std::string msg = JsToStdString(ctx, ex);

    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack))
    {
        std::string st = JsToStdString(ctx, stack);
        if (!st.empty())
            msg += "\n" + st;
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
    return msg;
}

static bool LooksLikeEsModule(const std::string& src)
{
    // Cheap heuristic: good enough to catch common ANSL examples.
    // (We still execute as a script; we just rewrite the trivial export forms.)
    return src.find("export ") != std::string::npos;
}

static std::string Trim(std::string s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static std::string ReplaceAllCopy(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static bool IsSupportedAnslModuleName(const std::string& name)
{
    // Keep this in sync with ansl/src/index.js exports (DOM-free only).
    return name == "buffer" ||
           name == "color" ||
           name == "drawbox" ||
           name == "num" ||
           name == "sdf" ||
           name == "string" ||
           name == "vec2" ||
           name == "vec3";
}

static std::string RewriteAnslImportsToGlobals(const std::string& src)
{
    // Compatibility shim for common ANSL programs that use absolute ESM imports like:
    //   import { map } from '/src/modules/num.js'
    //   import { sdCircle, opSmoothUnion } from '/src/modules/sdf.js'
    //   import * as v2 from '/src/modules/vec2.js'
    //
    // We rewrite these into:
    //   const { map } = ANSL.modules.num;
    //   const { sdCircle, opSmoothUnion } = ANSL.modules.sdf;
    //   const v2 = ANSL.modules.vec2;
    //
    // Limitations:
    // - Only supports '/src/modules/<name>.js' and './modules/<name>.js'
    // - Only supports named imports and namespace imports (`* as X`)
    // - Does not support default imports
    std::stringstream in(src);
    std::string line;
    std::string out;
    out.reserve(src.size());

    while (std::getline(in, line))
    {
        std::string raw = line;
        std::string s = Trim(line);

        auto starts_with = [&](const char* p) {
            return s.rfind(p, 0) == 0;
        };

        if (starts_with("import "))
        {
            // Find " from " and module path.
            size_t from_pos = s.find(" from ");
            if (from_pos != std::string::npos)
            {
                std::string lhs = Trim(s.substr(std::strlen("import "), from_pos - std::strlen("import ")));
                std::string rhs = Trim(s.substr(from_pos + std::strlen(" from ")));
                // rhs should end with ';' optionally.
                if (!rhs.empty() && rhs.back() == ';')
                    rhs.pop_back();
                rhs = Trim(rhs);

                auto strip_quotes = [&](std::string q) -> std::string {
                    if (q.size() >= 2 && ((q.front() == '\'' && q.back() == '\'') || (q.front() == '"' && q.back() == '"')))
                        return q.substr(1, q.size() - 2);
                    return {};
                };
                std::string path = strip_quotes(rhs);

                // Match /src/modules/<name>.js and ./modules/<name>.js
                const std::string p1 = "/src/modules/";
                const std::string p2 = "./modules/";
                std::string mod;
                if (path.rfind(p1, 0) == 0)
                    mod = path.substr(p1.size());
                else if (path.rfind(p2, 0) == 0)
                    mod = path.substr(p2.size());

                if (!mod.empty() && mod.size() > 3 && mod.substr(mod.size() - 3) == ".js")
                {
                    mod = mod.substr(0, mod.size() - 3);
                    if (IsSupportedAnslModuleName(mod))
                    {
                        // Namespace import: "* as v2"
                        if (lhs.rfind("* as ", 0) == 0)
                        {
                            std::string name = Trim(lhs.substr(std::strlen("* as ")));
                            out += "const " + name + " = ANSL.modules." + mod + ";\n";
                            continue;
                        }

                        // Named import: "{ a, b as c }"
                        if (!lhs.empty() && lhs.front() == '{' && lhs.back() == '}')
                        {
                            std::string named = Trim(lhs.substr(1, lhs.size() - 2));
                            // Convert " as " to ":" for object destructuring rename.
                            named = ReplaceAllCopy(named, " as ", ": ");
                            out += "const { " + named + " } = ANSL.modules." + mod + ";\n";
                            continue;
                        }
                    }
                }
            }
        }

        out += raw;
        out += "\n";
    }

    return out;
}

static std::string RewriteSimpleExportsToGlobals(const std::string& src)
{
    // This is intentionally a lightweight compatibility shim so users can paste
    // ANSL snippets that were authored as ESM modules.
    //
    // Supported forms:
    //   export const name = ...
    //   export let name = ...
    //   export var name = ...
    //   export function name(...) { ... }
    //
    // We rewrite these to assign on globalThis.* so they're visible from the host.
    //
    // NOTE: This is not a full JS parser. It won't handle every edge case.
    std::string out = src;

    auto replace_all = [&](const char* from, const char* to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos)
        {
            out.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    };

    replace_all("export const ", "globalThis.");
    replace_all("export let ", "globalThis.");
    replace_all("export var ", "globalThis.");
    replace_all("export function ", "globalThis.");

    // Turn "globalThis.name(" into "globalThis.name = function("
    // (only for the function form above). This is still heuristic.
    size_t pos = 0;
    while ((pos = out.find("globalThis.", pos)) != std::string::npos)
    {
        size_t name_start = pos + std::strlen("globalThis.");
        size_t name_end = name_start;
        while (name_end < out.size())
        {
            char c = out[name_end];
            if (!(std::isalnum((unsigned char)c) || c == '_' || c == '$'))
                break;
            ++name_end;
        }
        if (name_end < out.size() && out[name_end] == '(')
        {
            // Likely function form: globalThis.<name>( -> globalThis.<name> = function(
            out.insert(name_end, " = function");
            pos = name_end + std::strlen(" = function");
        }
        else
        {
            // const/let/var form already becomes globalThis.<name> = ...
            pos = name_end;
        }
    }

    return out;
}
} // namespace

struct AnslScriptEngine::Impl
{
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    JSValue render_fn = JS_UNDEFINED;
    std::string last_source;
    bool initialized = false;
};

AnslScriptEngine::AnslScriptEngine()
{
    impl_ = new Impl();
}

AnslScriptEngine::~AnslScriptEngine()
{
    if (!impl_)
        return;

    if (impl_->ctx)
    {
        JS_FreeValue(impl_->ctx, impl_->render_fn);
        JS_FreeContext(impl_->ctx);
    }
    if (impl_->rt)
        JS_FreeRuntime(impl_->rt);

    delete impl_;
    impl_ = nullptr;
}

bool AnslScriptEngine::Init(const std::string& assets_dir, std::string& error)
{
    if (!impl_)
        return false;
    if (impl_->initialized)
        return true;

    impl_->rt = JS_NewRuntime();
    if (!impl_->rt)
    {
        error = "JS_NewRuntime failed";
        return false;
    }
    impl_->ctx = JS_NewContext(impl_->rt);
    if (!impl_->ctx)
    {
        error = "JS_NewContext failed";
        return false;
    }

    // Basic global: print(...)
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, global, "print", JS_NewCFunction(impl_->ctx, JsPrint, "print", 1));

    // Basic console.log(...)
    JSValue console = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, console, "log", JS_NewCFunction(impl_->ctx, JsPrint, "log", 1));
    JS_SetPropertyStr(impl_->ctx, global, "console", console);
    JS_FreeValue(impl_->ctx, global);

    // Load assets/ansl.js (IIFE bundle that defines global ANSL).
    const std::string ansl_path = assets_dir + "/ansl.js";
    std::string src;
    if (!ReadFileToString(ansl_path, src, error))
        return false;

    JSValue r = JS_Eval(impl_->ctx, src.c_str(), src.size(), ansl_path.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r))
    {
        JS_FreeValue(impl_->ctx, r);
        error = FormatException(impl_->ctx);
        return false;
    }
    JS_FreeValue(impl_->ctx, r);

    impl_->initialized = true;
    error.clear();
    return true;
}

bool AnslScriptEngine::CompileUserScript(const std::string& source, std::string& error)
{
    if (!impl_ || !impl_->initialized)
    {
        error = "AnslScriptEngine not initialized";
        return false;
    }

    if (source == impl_->last_source && !JS_IsUndefined(impl_->render_fn))
        return true;

    impl_->last_source = source;
    JS_FreeValue(impl_->ctx, impl_->render_fn);
    impl_->render_fn = JS_UNDEFINED;

    // Evaluate user source as a global script. Users should define `function render(ctx, layer) { ... }`.
    std::string compiled_source = RewriteAnslImportsToGlobals(source);
    if (LooksLikeEsModule(compiled_source))
        compiled_source = RewriteSimpleExportsToGlobals(compiled_source);

    // Evaluate inside an IIFE so repeated recompiles don't collide on top-level `const`/`let`.
    // Then publish common entrypoints onto globalThis for the host.
    {
        std::string wrapped;
        wrapped.reserve(compiled_source.size() + 256);
        wrapped += "(function(){\n";
        wrapped += compiled_source;
        wrapped += "\n";
        wrapped += "if (typeof main === 'function') globalThis.main = main;\n";
        wrapped += "if (typeof render === 'function') globalThis.render = render;\n";
        wrapped += "if (typeof settings === 'object' && settings !== null) globalThis.settings = settings;\n";
        wrapped += "})();\n";
        compiled_source = std::move(wrapped);
    }

    JSValue r = JS_Eval(impl_->ctx,
                        compiled_source.c_str(),
                        compiled_source.size(),
                        "<ansl_editor>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r))
    {
        JS_FreeValue(impl_->ctx, r);
        error = FormatException(impl_->ctx);
        return false;
    }
    JS_FreeValue(impl_->ctx, r);

    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JSValue fn = JS_GetPropertyStr(impl_->ctx, global, "render");

    // Compatibility: if a classic ANSL `main()` exists (but no `render()`),
    // create a default render(ctx, layer) that calls main per cell.
    if (!JS_IsFunction(impl_->ctx, fn))
    {
        JS_FreeValue(impl_->ctx, fn);
        JSValue main_fn = JS_GetPropertyStr(impl_->ctx, global, "main");
        if (JS_IsFunction(impl_->ctx, main_fn))
        {
            JS_FreeValue(impl_->ctx, main_fn);
            const char* shim =
                "globalThis.render = function(ctx, layer) {\n"
                "  const cols = ctx.cols|0;\n"
                "  const rows = ctx.rows|0;\n"
                "  for (let y = 0; y < rows; y++) {\n"
                "    const arr = new Array(cols);\n"
                "    for (let x = 0; x < cols; x++) {\n"
                "      const idx = x + y * cols;\n"
                "      const out = globalThis.main({x, y, index: idx}, ctx, ctx.cursor || null, null);\n"
                "      arr[x] = (typeof out === 'string' ? out : String(out));\n"
                "    }\n"
                "    if (typeof layer.setRow === 'function') layer.setRow(y, arr.join(''));\n"
                "    else for (let x = 0; x < cols; x++) layer.set(x, y, arr[x] || ' ');\n"
                "  }\n"
                "};\n";
            JSValue shim_r = JS_Eval(impl_->ctx, shim, std::strlen(shim), "<ansl_shim>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(shim_r))
            {
                JS_FreeValue(impl_->ctx, shim_r);
                JS_FreeValue(impl_->ctx, global);
                error = FormatException(impl_->ctx);
                return false;
            }
            JS_FreeValue(impl_->ctx, shim_r);
            fn = JS_GetPropertyStr(impl_->ctx, global, "render");
        }
        else
        {
            JS_FreeValue(impl_->ctx, main_fn);
        }
    }

    JS_FreeValue(impl_->ctx, global);

    if (!JS_IsFunction(impl_->ctx, fn))
    {
        JS_FreeValue(impl_->ctx, fn);
        error =
            "Script must define either:\n"
            "  - function render(ctx, layer) { ... }\n"
            "or:\n"
            "  - function main(...) { ... }  (classic ANSL style; host will wrap it)";
        return false;
    }

    impl_->render_fn = fn; // keep
    error.clear();
    return true;
}

bool AnslScriptEngine::RunFrame(AnsiCanvas& canvas,
                               int layer_index,
                               const AnslFrameContext& frame_ctx,
                               bool clear_layer_first,
                               std::string& error)
{
    if (!impl_ || !impl_->initialized)
    {
        error = "AnslScriptEngine not initialized";
        return false;
    }
    if (JS_IsUndefined(impl_->render_fn))
    {
        error = "No render() function compiled";
        return false;
    }

    if (clear_layer_first)
        canvas.ClearLayer(layer_index, U' ');

    JSValue ctx_obj = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "cols", JS_NewInt32(impl_->ctx, frame_ctx.cols));
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "rows", JS_NewInt32(impl_->ctx, frame_ctx.rows));
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "frame", JS_NewInt32(impl_->ctx, frame_ctx.frame));
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "time", JS_NewFloat64(impl_->ctx, frame_ctx.time));

    // context.metrics.aspect (classic ANSL runner compatibility)
    JSValue metrics_obj = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, metrics_obj, "aspect", JS_NewFloat64(impl_->ctx, (double)frame_ctx.metrics_aspect));
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "metrics", metrics_obj); // transfers ownership

    // cursor object (classic ANSL runner compatibility)
    JSValue cursor_obj = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, cursor_obj, "x", JS_NewInt32(impl_->ctx, frame_ctx.cursor_x));
    JS_SetPropertyStr(impl_->ctx, cursor_obj, "y", JS_NewInt32(impl_->ctx, frame_ctx.cursor_y));
    JS_SetPropertyStr(impl_->ctx, cursor_obj, "pressed", JS_NewBool(impl_->ctx, frame_ctx.cursor_pressed));
    JSValue cursor_prev = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, cursor_prev, "x", JS_NewInt32(impl_->ctx, frame_ctx.cursor_px));
    JS_SetPropertyStr(impl_->ctx, cursor_prev, "y", JS_NewInt32(impl_->ctx, frame_ctx.cursor_py));
    JS_SetPropertyStr(impl_->ctx, cursor_prev, "pressed", JS_NewBool(impl_->ctx, frame_ctx.cursor_ppressed));
    JS_SetPropertyStr(impl_->ctx, cursor_obj, "p", cursor_prev); // transfers ownership
    JS_SetPropertyStr(impl_->ctx, ctx_obj, "cursor", cursor_obj); // transfers ownership

    JSValue layer_obj = NewLayerObject(impl_->ctx, &canvas, layer_index);

    JSValue args[2] = { ctx_obj, layer_obj };
    JSValue ret = JS_Call(impl_->ctx, impl_->render_fn, JS_UNDEFINED, 2, args);

    if (JS_IsException(ret))
    {
        JS_FreeValue(impl_->ctx, ret);
        JS_FreeValue(impl_->ctx, layer_obj);
        JS_FreeValue(impl_->ctx, ctx_obj);
        error = FormatException(impl_->ctx);
        return false;
    }

    JS_FreeValue(impl_->ctx, ret);
    JS_FreeValue(impl_->ctx, layer_obj);
    JS_FreeValue(impl_->ctx, ctx_obj);

    error.clear();
    return true;
}

bool AnslScriptEngine::HasRenderFunction() const
{
    return impl_ && impl_->initialized && !JS_IsUndefined(impl_->render_fn);
}


