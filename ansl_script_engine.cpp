#include "ansl_script_engine.h"

#include "canvas.h"
#include "xterm256_palette.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <vector>
#include <string>

namespace
{
// Forward declaration (used by layer bindings before the definition below).
static bool ParseHexColorToXtermIndex(const std::string& s, int& out_idx);

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

static std::string EncodeCodepointUtf8(char32_t cp)
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

static std::string LuaToString(lua_State* L, int idx)
{
    size_t len = 0;
    const char* s = lua_tolstring(L, idx, &len);
    if (!s)
        return {};
    return std::string(s, len);
}

static char32_t LuaCharArg(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx))
    {
        lua_Integer cp = lua_tointeger(L, idx);
        if (cp < 0) cp = 0;
        return static_cast<char32_t>(cp);
    }

    size_t len = 0;
    const char* s = lua_tolstring(L, idx, &len);
    if (!s)
        return U' ';
    return DecodeFirstUtf8Codepoint(s, len);
}

struct LayerBinding
{
    AnsiCanvas* canvas = nullptr;
    int layer_index = 0;
};

static LayerBinding* CheckLayer(lua_State* L, int idx)
{
    void* ud = luaL_checkudata(L, idx, "AnsiLayer");
    return static_cast<LayerBinding*>(ud);
}

static int l_layer_set(lua_State* L)
{
    LayerBinding* b = CheckLayer(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid layer binding");

    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    const char32_t cp = LuaCharArg(L, 4);

    // Optional fg/bg: xterm-256 indices (0..255). nil means "unset".
    AnsiCanvas::Color32 fg = 0;
    AnsiCanvas::Color32 bg = 0;
    const int nargs = lua_gettop(L);
    if (nargs >= 5 && !lua_isnil(L, 5))
    {
        const int idx = (int)luaL_checkinteger(L, 5);
        if (idx >= 0 && idx <= 255)
            fg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(idx);
    }
    if (nargs >= 6 && !lua_isnil(L, 6))
    {
        const int idx = (int)luaL_checkinteger(L, 6);
        if (idx >= 0 && idx <= 255)
            bg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(idx);
    }

    if (fg != 0 || bg != 0)
        b->canvas->SetLayerCell(b->layer_index, y, x, cp, fg, bg);
    else
        b->canvas->SetLayerCell(b->layer_index, y, x, cp);
    return 0;
}

static int l_layer_get(lua_State* L)
{
    LayerBinding* b = CheckLayer(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid layer binding");

    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    const char32_t cp = b->canvas->GetLayerCell(b->layer_index, y, x);
    const std::string s = EncodeCodepointUtf8(cp);
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

static int l_layer_clear(lua_State* L)
{
    LayerBinding* b = CheckLayer(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid layer binding");

    char32_t fill = U' ';
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        fill = LuaCharArg(L, 2);
    b->canvas->ClearLayer(b->layer_index, fill);

    // Optional fg/bg can be passed explicitly:
    //   layer:clear(cpOrString?, fg?, bg?)
    // Where fg/bg are xterm-256 indices or "#RRGGBB".
    // If fg/bg are omitted, we fall back to global `settings.fg`/`settings.bg` if present.
    std::optional<AnsiCanvas::Color32> fg;
    std::optional<AnsiCanvas::Color32> bg;

    auto parseColorValueAt = [&](int idx, std::optional<AnsiCanvas::Color32>& out) -> void {
        if (lua_gettop(L) < idx || lua_isnil(L, idx))
            return;
        int xidx = -1;
        if (lua_isnumber(L, idx))
        {
            xidx = std::clamp((int)lua_tointeger(L, idx), 0, 255);
        }
        else if (lua_isstring(L, idx))
        {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            int parsed = 0;
            if (s && ParseHexColorToXtermIndex(std::string(s, s + len), parsed))
                xidx = std::clamp(parsed, 0, 255);
        }
        if (xidx >= 0)
            out = (AnsiCanvas::Color32)xterm256::Color32ForIndex(xidx);
    };

    parseColorValueAt(3, fg);
    parseColorValueAt(4, bg);

    // If fg/bg weren't provided, try settings = { fg=..., bg=... }.
    if (!fg.has_value() && !bg.has_value())
    {
        lua_getglobal(L, "settings");
        if (lua_istable(L, -1))
        {
            auto parseSettingField = [&](const char* const* keys, std::optional<AnsiCanvas::Color32>& out) -> void {
                for (int i = 0; keys[i] != nullptr; ++i)
                {
                    lua_getfield(L, -1, keys[i]);
                    if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0))
                    {
                        lua_pop(L, 1);
                        continue;
                    }
                    int xidx = -1;
                    if (lua_isnumber(L, -1))
                    {
                        xidx = std::clamp((int)lua_tointeger(L, -1), 0, 255);
                    }
                    else if (lua_isstring(L, -1))
                    {
                        size_t len = 0;
                        const char* s = lua_tolstring(L, -1, &len);
                        int parsed = 0;
                        if (s && ParseHexColorToXtermIndex(std::string(s, s + len), parsed))
                            xidx = std::clamp(parsed, 0, 255);
                    }
                    lua_pop(L, 1);
                    if (xidx >= 0)
                    {
                        out = (AnsiCanvas::Color32)xterm256::Color32ForIndex(xidx);
                        return;
                    }
                }
            };

            const char* fg_keys[] = {"fg", "foreground", "foregroundColor", nullptr};
            const char* bg_keys[] = {"bg", "background", "backgroundColor", nullptr};
            parseSettingField(fg_keys, fg);
            parseSettingField(bg_keys, bg);
        }
        lua_pop(L, 1); // settings or non-table
    }

    if (fg.has_value() || bg.has_value())
        b->canvas->FillLayer(b->layer_index, std::nullopt, fg, bg);
    return 0;
}

static int l_layer_setRow(lua_State* L)
{
    LayerBinding* b = CheckLayer(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid layer binding");

    int y = (int)luaL_checkinteger(L, 2);
    if (y < 0) y = 0;
    size_t len = 0;
    const char* s = luaL_checklstring(L, 3, &len);

    std::vector<char32_t> cps;
    DecodeUtf8ToCodepoints(s, len, cps);

    const int cols = b->canvas->GetColumns();
    b->canvas->EnsureRowsPublic(y + 1);
    for (int x = 0; x < cols; ++x)
    {
        const char32_t cp = (x < (int)cps.size()) ? cps[(size_t)x] : U' ';
        b->canvas->SetLayerCell(b->layer_index, y, x, cp);
    }
    return 0;
}

static int l_layer_clearStyle(lua_State* L)
{
    LayerBinding* b = CheckLayer(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid layer binding");
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    b->canvas->ClearLayerCellStyle(b->layer_index, y, x);
    return 0;
}

static int l_layer_gc(lua_State* L)
{
    LayerBinding* b = static_cast<LayerBinding*>(luaL_checkudata(L, 1, "AnsiLayer"));
    if (b)
    {
        b->canvas = nullptr;
        b->layer_index = 0;
    }
    return 0;
}

static void EnsureLayerMetatable(lua_State* L)
{
    if (luaL_newmetatable(L, "AnsiLayer"))
    {
        lua_pushcfunction(L, l_layer_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L); // __index
        lua_pushcfunction(L, l_layer_set);
        lua_setfield(L, -2, "set");
        lua_pushcfunction(L, l_layer_get);
        lua_setfield(L, -2, "get");
        lua_pushcfunction(L, l_layer_clear);
        lua_setfield(L, -2, "clear");
        lua_pushcfunction(L, l_layer_setRow);
        lua_setfield(L, -2, "setRow");
        lua_pushcfunction(L, l_layer_clearStyle);
        lua_setfield(L, -2, "clearStyle");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1); // metatable
}

static void PushLayerObject(lua_State* L, AnsiCanvas* canvas, int layer_index)
{
    EnsureLayerMetatable(L);
    auto* b = static_cast<LayerBinding*>(lua_newuserdata(L, sizeof(LayerBinding)));
    b->canvas = canvas;
    b->layer_index = layer_index;
    luaL_setmetatable(L, "AnsiLayer");
}

static int LuaTraceback(lua_State* L)
{
    const char* msg = lua_tostring(L, 1);
    if (!msg)
        msg = "error";
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static bool PCallWithTraceback(lua_State* L, int nargs, int nresults, std::string& error)
{
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, LuaTraceback);
    lua_insert(L, base);
    const int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (status != LUA_OK)
    {
        error = LuaToString(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static bool PCallNoTraceback(lua_State* L, int nargs, int nresults, std::string& error)
{
    const int status = lua_pcall(L, nargs, nresults, 0);
    if (status != LUA_OK)
    {
        error = LuaToString(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

extern "C" int luaopen_ansl(lua_State* L);

static bool EnsureAnslModule(lua_State* L, std::string& error)
{
    // require('ansl') and also publish global `ansl` for convenience.
    // LuaJIT (Lua 5.1) does not provide luaL_requiref, so we do the equivalent:
    // - register package.preload["ansl"] = luaopen_ansl
    // - call require("ansl")
    // - assign global ansl = returned module table
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1))
                        {
        lua_pop(L, 1);
        error = "Lua: global 'package' is not a table";
        return false;
}
    lua_getfield(L, -1, "preload");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 2);
        error = "Lua: package.preload is not a table";
        return false;
    }
    lua_pushcfunction(L, luaopen_ansl);
    lua_setfield(L, -2, "ansl"); // preload["ansl"] = luaopen_ansl
    lua_pop(L, 2); // pop preload, package

    // Call require("ansl") with traceback.
    lua_settop(L, 0);
    lua_getglobal(L, "require");
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        error = "Lua: global 'require' is not a function";
        return false;
    }
    lua_pushstring(L, "ansl");
    if (!PCallWithTraceback(L, 1, 1, error))
        return false;

    // stack: ansl_module
    lua_pushvalue(L, -1);
    lua_setglobal(L, "ansl");
    lua_pop(L, 1);
    error.clear();
    return true;
}

static bool ParseHexColorToXtermIndex(const std::string& s, int& out_idx)
{
    // Accept "#RRGGBB" or "RRGGBB".
    std::string str = s;
    if (!str.empty() && str[0] == '#')
        str.erase(0, 1);
    if (str.size() != 6)
        return false;
    for (char ch : str)
    {
        if (!std::isxdigit((unsigned char)ch))
            return false;
    }

    auto byte = [&](int off) -> int {
        return (int)std::strtoul(str.substr((size_t)off, 2).c_str(), nullptr, 16);
    };
    const int r = std::clamp(byte(0), 0, 255);
    const int g = std::clamp(byte(2), 0, 255);
    const int b = std::clamp(byte(4), 0, 255);
    out_idx = xterm256::NearestIndex((std::uint8_t)r, (std::uint8_t)g, (std::uint8_t)b);
    return true;
}

static void ReadScriptSettings(lua_State* L, AnslScriptSettings& out)
{
    out = AnslScriptSettings{};

    lua_getglobal(L, "settings");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return;
    }

    // fps
    lua_getfield(L, -1, "fps");
    if (lua_isnumber(L, -1))
    {
        int fps = (int)lua_tointeger(L, -1);
        fps = std::clamp(fps, 1, 240);
        out.has_fps = true;
        out.fps = fps;
    }
    lua_pop(L, 1);

    // once
    lua_getfield(L, -1, "once");
    if (!lua_isnil(L, -1))
        out.once = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    auto parse_color_field = [&](const char* const* keys, int& out_idx) -> bool
    {
        for (const char* k = *keys; k; ++keys, k = *keys)
        {
            lua_getfield(L, -1, k);
            if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0))
            {
                lua_pop(L, 1);
                continue;
            }

            if (lua_isnumber(L, -1))
            {
                int idx = (int)lua_tointeger(L, -1);
                out_idx = std::clamp(idx, 0, 255);
                lua_pop(L, 1);
                return true;
            }

            if (lua_isstring(L, -1))
            {
                size_t len = 0;
                const char* s = lua_tolstring(L, -1, &len);
                int idx = 0;
                if (s && ParseHexColorToXtermIndex(std::string(s, s + len), idx))
                {
                    out_idx = std::clamp(idx, 0, 255);
                    lua_pop(L, 1);
                    return true;
                }
            }

            lua_pop(L, 1);
        }
        return false;
    };

    // Foreground: fg / foreground (preferred) + some aliases.
    // - number: xterm-256 index (0..255)
    // - string: "#RRGGBB" or "RRGGBB" (converted to nearest xterm-256)
    const char* fg_keys[] = {"fg", "foreground", "foregroundColor", nullptr};
    int fg_idx = 0;
    if (parse_color_field(fg_keys, fg_idx))
    {
        out.has_foreground = true;
        out.foreground_xterm = fg_idx;
    }

    // Background: bg / background (preferred) + some aliases.
    const char* bg_keys[] = {"bg", "background", "backgroundColor", nullptr};
    int bg_idx = 0;
    if (parse_color_field(bg_keys, bg_idx))
    {
        out.has_background = true;
        out.background_xterm = bg_idx;
    }

    lua_pop(L, 1); // settings table
}
} // namespace

struct AnslScriptEngine::Impl
{
    lua_State* L = nullptr;
    int render_ref = LUA_NOREF;
    int ctx_ref = LUA_NOREF; // reusable ctx table (with nested metrics/cursor/p tables)
    std::string last_source;
    bool initialized = false;
    AnslScriptSettings settings;
};

AnslScriptEngine::AnslScriptEngine()
{
    impl_ = new Impl();
}

AnslScriptEngine::~AnslScriptEngine()
{
    if (!impl_)
        return;

    if (impl_->L)
    {
        if (impl_->render_ref != LUA_NOREF)
            luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->render_ref);
        if (impl_->ctx_ref != LUA_NOREF)
            luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->ctx_ref);
        lua_close(impl_->L);
    }

    delete impl_;
    impl_ = nullptr;
}

bool AnslScriptEngine::Init(const std::string& assets_dir, std::string& error)
{
    (void)assets_dir;
    if (!impl_)
        return false;
    if (impl_->initialized)
        return true;

    impl_->L = luaL_newstate();
    if (!impl_->L)
    {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(impl_->L);

    // Register native ansl library as require('ansl') and global `ansl`.
    if (!EnsureAnslModule(impl_->L, error))
        return false;

    // Pre-create a reusable ctx table to avoid per-frame allocations/GC churn.
    // ctx = {
    //   cols, rows, frame, time, fg, bg,
    //   focused, phase,
    //   keys={...}, typed={...},
    //   metrics={aspect=...},
    //   caret={x,y},
    //   cursor={valid,x,y,left,right,p={x,y,left,right}}
    // }
    lua_newtable(impl_->L); // ctx
    lua_newtable(impl_->L); // metrics
    lua_pushnumber(impl_->L, 1.0);
    lua_setfield(impl_->L, -2, "aspect");
    lua_setfield(impl_->L, -2, "metrics"); // ctx.metrics = metrics

    lua_newtable(impl_->L); // cursor
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "valid");
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "x");
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "y");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "left");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "right");
    lua_newtable(impl_->L); // cursor.p
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "x");
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "y");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "left");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "right");
    lua_setfield(impl_->L, -2, "p");
    lua_setfield(impl_->L, -2, "cursor"); // ctx.cursor = cursor

    lua_newtable(impl_->L); // caret
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "x");
    lua_pushinteger(impl_->L, 0); lua_setfield(impl_->L, -2, "y");
    lua_setfield(impl_->L, -2, "caret"); // ctx.caret = caret

    // keys table (reused)
    lua_newtable(impl_->L); // keys
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "left");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "right");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "up");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "down");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "home");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "end");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "backspace");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "delete");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "enter");
    lua_setfield(impl_->L, -2, "keys"); // ctx.keys = keys

    impl_->ctx_ref = luaL_ref(impl_->L, LUA_REGISTRYINDEX);

    impl_->initialized = true;
    impl_->settings = AnslScriptSettings{};
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

    if (source == impl_->last_source && impl_->render_ref != LUA_NOREF)
        return true;

    impl_->last_source = source;
    impl_->settings = AnslScriptSettings{};
    if (impl_->render_ref != LUA_NOREF)
    {
        luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->render_ref);
        impl_->render_ref = LUA_NOREF;
    }

    // Evaluate user source as a chunk. Users should define global:
    // - function render(ctx, layer) ... end
    // or classic: function main(coord, context, cursor, buffer) ... end (host wraps)
    lua_settop(impl_->L, 0);
    if (luaL_loadbuffer(impl_->L, source.c_str(), source.size(), "<ansl_editor>") != LUA_OK)
    {
        error = LuaToString(impl_->L, -1);
        lua_pop(impl_->L, 1);
        return false;
    }
    if (!PCallWithTraceback(impl_->L, 0, 0, error))
        return false;

    // If render is missing but main exists, create a compatibility render().
    lua_getglobal(impl_->L, "render");
    const bool has_render = lua_isfunction(impl_->L, -1);
    lua_pop(impl_->L, 1);
    if (!has_render)
    {
        lua_getglobal(impl_->L, "main");
        const bool has_main = lua_isfunction(impl_->L, -1);
        lua_pop(impl_->L, 1);
        if (has_main)
        {
            static const char* shim =
                "if type(render) ~= 'function' and type(main) == 'function' then\n"
                "  local __ansl_buf, __ansl_cols, __ansl_rows\n"
                "  local function __ansl_ensure_buf(cols, rows)\n"
                "    if not __ansl_buf then __ansl_buf = {} end\n"
                "    __ansl_cols, __ansl_rows = cols, rows\n"
                "    local n = cols * rows\n"
                "    for i = #__ansl_buf, n + 1, -1 do __ansl_buf[i] = nil end\n"
                "    return __ansl_buf\n"
                "  end\n"
                "  function render(ctx, layer)\n"
                "    local cols = tonumber(ctx.cols) or 0\n"
                "    local rows = tonumber(ctx.rows) or 0\n"
                "    if cols <= 0 or rows <= 0 then return end\n"
                "    local cursor = ctx.cursor\n"
                "    local buf = __ansl_buf\n"
                "    if (not buf) or __ansl_cols ~= cols or __ansl_rows ~= rows then\n"
                "      buf = __ansl_ensure_buf(cols, rows)\n"
                "    end\n"
                "    local pre = rawget(_G, 'pre')\n"
                "    if type(pre) == 'function' then pre(ctx, cursor, buf) end\n"
                "    for y = 0, rows - 1 do\n"
                "      local row = {}\n"
                "      local anyStyle = false\n"
                "      for x = 0, cols - 1 do\n"
                "        local idx = x + y * cols\n"
                "        local out = main({x = x, y = y, index = idx}, ctx, cursor, buf)\n"
                "        if type(out) == 'table' then\n"
                "          local ch = out.char\n"
                "          if ch == nil then ch = out[1] end\n"
                "          if ch == nil then ch = ' ' end\n"
                "          if type(ch) == 'number' then ch = tostring(ch) end\n"
                "          local fg = out.fg; if fg == nil then fg = out.color end\n"
                "          local bg = out.bg; if bg == nil then bg = out.backgroundColor end\n"
                "          if type(fg) ~= 'number' then fg = nil end\n"
                "          if type(bg) ~= 'number' then bg = nil end\n"
                "          if fg ~= nil or bg ~= nil then\n"
                "            anyStyle = true\n"
                "            layer:set(x, y, ch, fg, bg)\n"
                "          else\n"
                "            row[x + 1] = tostring(ch)\n"
                "          end\n"
                "        else\n"
                "          if type(out) == 'number' then out = tostring(out) end\n"
                "          row[x + 1] = tostring(out)\n"
                "        end\n"
                "      end\n"
                "      if anyStyle then\n"
                "        for x = 0, cols - 1 do\n"
                "          local s = row[x + 1]\n"
                "          if s ~= nil then layer:set(x, y, s) end\n"
                "        end\n"
                "      else\n"
                "        layer:setRow(y, table.concat(row))\n"
                "      end\n"
                "    end\n"
                "    local post = rawget(_G, 'post')\n"
                "    if type(post) == 'function' then post(ctx, cursor, buf) end\n"
                "  end\n"
                "end\n";
            if (luaL_loadbuffer(impl_->L, shim, std::strlen(shim), "<ansl_shim>") != LUA_OK)
            {
                error = LuaToString(impl_->L, -1);
                lua_pop(impl_->L, 1);
                return false;
            }
            if (!PCallWithTraceback(impl_->L, 0, 0, error))
                return false;
        }
    }

    lua_getglobal(impl_->L, "render");
    if (!lua_isfunction(impl_->L, -1))
    {
        lua_pop(impl_->L, 1);
        error =
            "Script must define either:\n"
            "  - function render(ctx, layer) ... end\n"
            "or:\n"
            "  - function main(coord, context, cursor, buffer) ... end  (classic ANSL style; host will wrap it)";
        return false;
    }
    impl_->render_ref = luaL_ref(impl_->L, LUA_REGISTRYINDEX);

    // Read global `settings` table (optional).
    ReadScriptSettings(impl_->L, impl_->settings);
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
    if (impl_->render_ref == LUA_NOREF)
    {
        error = "No render() function compiled";
        return false;
    }

    if (clear_layer_first)
    {
        canvas.ClearLayer(layer_index, U' ');
        // If the script requested a fg/bg fill, apply it after clearing.
        // This keeps defaults stable even when "Clear layer each frame" is enabled.
        std::optional<AnsiCanvas::Color32> fg;
        std::optional<AnsiCanvas::Color32> bg;
        if (impl_->settings.has_foreground)
            fg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(impl_->settings.foreground_xterm);
        if (impl_->settings.has_background)
            bg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(impl_->settings.background_xterm);
        if (fg.has_value() || bg.has_value())
            canvas.FillLayer(layer_index, std::nullopt, fg, bg);
    }

    lua_State* L = impl_->L;
    lua_settop(L, 0);

    // Push render(ctx, layer)
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->render_ref);

    // ctx table (reused)
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->ctx_ref); // stack: render, ctx
    lua_pushinteger(L, frame_ctx.cols);  lua_setfield(L, -2, "cols");
    lua_pushinteger(L, frame_ctx.rows);  lua_setfield(L, -2, "rows");
    lua_pushinteger(L, frame_ctx.frame); lua_setfield(L, -2, "frame");
    lua_pushnumber(L, frame_ctx.time);   lua_setfield(L, -2, "time");
    lua_pushboolean(L, frame_ctx.focused ? 1 : 0); lua_setfield(L, -2, "focused");
    lua_pushinteger(L, frame_ctx.phase); lua_setfield(L, -2, "phase");
    if (frame_ctx.fg >= 0) { lua_pushinteger(L, frame_ctx.fg); lua_setfield(L, -2, "fg"); }
    else { lua_pushnil(L); lua_setfield(L, -2, "fg"); }
    if (frame_ctx.bg >= 0) { lua_pushinteger(L, frame_ctx.bg); lua_setfield(L, -2, "bg"); }
    else { lua_pushnil(L); lua_setfield(L, -2, "bg"); }

    lua_getfield(L, -1, "metrics");
    if (lua_istable(L, -1))
    {
        lua_pushnumber(L, (lua_Number)frame_ctx.metrics_aspect);
        lua_setfield(L, -2, "aspect");
    }
    lua_pop(L, 1); // metrics

    lua_getfield(L, -1, "caret");
    if (lua_istable(L, -1))
    {
        lua_pushinteger(L, frame_ctx.caret_x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, frame_ctx.caret_y); lua_setfield(L, -2, "y");
    }
    lua_pop(L, 1); // caret

    // keys table
    lua_getfield(L, -1, "keys");
    if (lua_istable(L, -1))
    {
        lua_pushboolean(L, frame_ctx.key_left);      lua_setfield(L, -2, "left");
        lua_pushboolean(L, frame_ctx.key_right);     lua_setfield(L, -2, "right");
        lua_pushboolean(L, frame_ctx.key_up);        lua_setfield(L, -2, "up");
        lua_pushboolean(L, frame_ctx.key_down);      lua_setfield(L, -2, "down");
        lua_pushboolean(L, frame_ctx.key_home);      lua_setfield(L, -2, "home");
        lua_pushboolean(L, frame_ctx.key_end);       lua_setfield(L, -2, "end");
        lua_pushboolean(L, frame_ctx.key_backspace); lua_setfield(L, -2, "backspace");
        lua_pushboolean(L, frame_ctx.key_delete);    lua_setfield(L, -2, "delete");
        lua_pushboolean(L, frame_ctx.key_enter);     lua_setfield(L, -2, "enter");
    }
    lua_pop(L, 1); // keys

    // typed codepoints -> ctx.typed = { "a", "中", ... }
    lua_newtable(L);
    if (frame_ctx.typed)
    {
        const auto& cps = *frame_ctx.typed;
        for (size_t i = 0; i < cps.size(); ++i)
        {
            const std::string ch = EncodeCodepointUtf8(cps[i]);
            lua_pushlstring(L, ch.data(), ch.size());
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
    }
    lua_setfield(L, -2, "typed");

    lua_getfield(L, -1, "cursor");
    if (lua_istable(L, -1))
    {
        lua_pushboolean(L, frame_ctx.cursor_valid ? 1 : 0); lua_setfield(L, -2, "valid");
        lua_pushinteger(L, frame_ctx.cursor_x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, frame_ctx.cursor_y); lua_setfield(L, -2, "y");
        lua_pushboolean(L, frame_ctx.cursor_left_down); lua_setfield(L, -2, "left");
        lua_pushboolean(L, frame_ctx.cursor_right_down); lua_setfield(L, -2, "right");

        lua_getfield(L, -1, "p");
        if (lua_istable(L, -1))
        {
            lua_pushinteger(L, frame_ctx.cursor_px); lua_setfield(L, -2, "x");
            lua_pushinteger(L, frame_ctx.cursor_py); lua_setfield(L, -2, "y");
            lua_pushboolean(L, frame_ctx.cursor_prev_left_down); lua_setfield(L, -2, "left");
            lua_pushboolean(L, frame_ctx.cursor_prev_right_down); lua_setfield(L, -2, "right");
        }
        lua_pop(L, 1); // p
    }
    lua_pop(L, 1); // cursor

    // layer userdata
    PushLayerObject(L, &canvas, layer_index);

    // Hot path: avoid generating stack traces every frame (can be expensive).
    // We still return the error message string.
    if (!PCallNoTraceback(L, 2, 0, error))
        return false;

    // Tool support: allow scripts to write caret back via ctx.caret.{x,y}.
    if (frame_ctx.allow_caret_writeback)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->ctx_ref);
        lua_getfield(L, -1, "caret");
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "x");
            lua_getfield(L, -2, "y");
            if (lua_isnumber(L, -2) && lua_isnumber(L, -1))
            {
                const int x = (int)lua_tointeger(L, -2);
                const int y = (int)lua_tointeger(L, -1);
                canvas.SetCaretCell(x, y);
            }
            lua_pop(L, 2); // x,y
        }
        lua_pop(L, 2); // caret, ctx
    }

    error.clear();
    return true;
}

bool AnslScriptEngine::HasRenderFunction() const
{
    return impl_ && impl_->initialized && impl_->render_ref != LUA_NOREF;
}

AnslScriptSettings AnslScriptEngine::GetSettings() const
{
    if (!impl_ || !impl_->initialized)
        return AnslScriptSettings{};
    return impl_->settings;
}


