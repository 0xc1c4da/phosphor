#include "ansl/ansl_script_engine.h"

#include "core/canvas.h"
#include "core/xterm256_palette.h"
#include "fonts/textmode_font_registry.h"

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
#include <cmath>
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

static int Color32ToXtermIndex(AnsiCanvas::Color32 c32)
{
    if (c32 == 0)
        return -1;

    // xterm256::Color32ForIndex() is documented as ABGR (A=255) where the low byte is R.
    // Even if callers store non-xterm colors, we still want tools to be able to pick a reasonable
    // palette index, so we compute the nearest xterm-256 index.
    const std::uint8_t r = (std::uint8_t)((c32 >> 0) & 0xFF);
    const std::uint8_t g = (std::uint8_t)((c32 >> 8) & 0xFF);
    const std::uint8_t b = (std::uint8_t)((c32 >> 16) & 0xFF);
    return xterm256::NearestIndex(r, g, b);
}

static int LuaArrayLen(lua_State* L, int idx)
{
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 502
    return (int)lua_rawlen(L, idx);
#else
    // LuaJIT (Lua 5.1 API)
    return (int)lua_objlen(L, idx);
#endif
}

struct LayerBinding
{
    AnsiCanvas* canvas = nullptr;
    int layer_index = 0;
};

struct CanvasBinding
{
    AnsiCanvas* canvas = nullptr;
};

static LayerBinding* CheckLayer(lua_State* L, int idx)
{
    void* ud = luaL_checkudata(L, idx, "AnsiLayer");
    return static_cast<LayerBinding*>(ud);
}

static CanvasBinding* CheckCanvas(lua_State* L, int idx)
{
    void* ud = luaL_checkudata(L, idx, "AnsiCanvas");
    return static_cast<CanvasBinding*>(ud);
}

static int l_canvas_hasSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    lua_pushboolean(L, (b && b->canvas && b->canvas->HasSelection()) ? 1 : 0);
    return 1;
}

static int l_canvas_getSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas || !b->canvas->HasSelection())
    {
        lua_pushnil(L);
        return 1;
    }
    const auto r = b->canvas->GetSelectionRect();
    lua_pushinteger(L, r.x);
    lua_pushinteger(L, r.y);
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 4;
}

static int l_canvas_getCell(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");

    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);

    // canvas:getCell(x, y, [mode], [layer])
    // mode:
    //   - "composite" (default): visible composited cell
    //   - "layer": raw cell from a specific layer (defaults to active layer)
    std::string mode;
    int layer = -9999;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4))
    {
        if (lua_isstring(L, 4))
            mode = LuaToString(L, 4);
        else if (lua_isnumber(L, 4))
            layer = (int)lua_tointeger(L, 4);
    }
    if (lua_gettop(L) >= 5 && !lua_isnil(L, 5))
        layer = (int)luaL_checkinteger(L, 5);

    const bool want_layer = (mode == "layer" || mode == "Layer");
    char32_t cp = U' ';
    AnsiCanvas::Color32 fg32 = 0;
    AnsiCanvas::Color32 bg32 = 0;

    if (want_layer || layer != -9999)
    {
        const int li = (layer == -9999) ? b->canvas->GetActiveLayerIndex() : layer;
        cp = b->canvas->GetLayerCell(li, y, x);
        (void)b->canvas->GetLayerCellColors(li, y, x, fg32, bg32);
    }
    else
    {
        // Composite is bounded and returns false if out-of-range.
        if (!b->canvas->GetCompositeCellPublic(y, x, cp, fg32, bg32))
        {
            cp = U' ';
            fg32 = 0;
            bg32 = 0;
        }
    }

    const std::string s = EncodeCodepointUtf8(cp);
    lua_pushlstring(L, s.data(), s.size());

    const int fg_idx = Color32ToXtermIndex(fg32);
    const int bg_idx = Color32ToXtermIndex(bg32);
    if (fg_idx >= 0) lua_pushinteger(L, fg_idx);
    else lua_pushnil(L);
    if (bg_idx >= 0) lua_pushinteger(L, bg_idx);
    else lua_pushnil(L);

    // Also return cp as an integer (handy for tools; safe additive API).
    lua_pushinteger(L, (lua_Integer)cp);
    return 4;
}

static int l_canvas_setSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int x0 = (int)luaL_checkinteger(L, 2);
    const int y0 = (int)luaL_checkinteger(L, 3);
    const int x1 = (int)luaL_checkinteger(L, 4);
    const int y1 = (int)luaL_checkinteger(L, 5);
    b->canvas->SetSelectionCorners(x0, y0, x1, y1);
    return 0;
}

static int l_canvas_clearSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    b->canvas->ClearSelection();
    return 0;
}

static int l_canvas_selectionContains(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    lua_pushboolean(L, b->canvas->SelectionContains(x, y) ? 1 : 0);
    return 1;
}

static int l_canvas_clipboardHas(lua_State* L)
{
    lua_pushboolean(L, AnsiCanvas::ClipboardHas() ? 1 : 0);
    return 1;
}

static int l_canvas_clipboardSize(lua_State* L)
{
    if (!AnsiCanvas::ClipboardHas())
    {
        lua_pushnil(L);
        return 1;
    }
    const auto r = AnsiCanvas::ClipboardRect();
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 2;
}

static int LuaOptLayerIndex(lua_State* L, int idx, int def = -1)
{
    if (lua_gettop(L) < idx || lua_isnil(L, idx))
        return def;
    return (int)luaL_checkinteger(L, idx);
}

static AnsiCanvas::PasteMode ParsePasteMode(lua_State* L, int idx, AnsiCanvas::PasteMode def = AnsiCanvas::PasteMode::Both)
{
    if (lua_gettop(L) < idx || lua_isnil(L, idx))
        return def;
    if (lua_isnumber(L, idx))
    {
        const int v = (int)lua_tointeger(L, idx);
        if (v == 1) return AnsiCanvas::PasteMode::CharOnly;
        if (v == 2) return AnsiCanvas::PasteMode::ColorOnly;
        return AnsiCanvas::PasteMode::Both;
    }
    if (lua_isstring(L, idx))
    {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        const std::string v(s ? s : "", s ? (s + len) : (s ? s : ""));
        if (v == "char" || v == "Char" || v == "glyph" || v == "charOnly" || v == "CharOnly")
            return AnsiCanvas::PasteMode::CharOnly;
        if (v == "color" || v == "colour" || v == "colorOnly" || v == "ColorOnly")
            return AnsiCanvas::PasteMode::ColorOnly;
        if (v == "both" || v == "Both")
            return AnsiCanvas::PasteMode::Both;
    }
    return def;
}

static int l_canvas_copySelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    // copySelection([modeOrLayer], [layer])
    // mode: "layer" (default) or "composite"
    int layer = -1;
    bool composite = false;
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2))
    {
        const std::string mode = LuaToString(L, 2);
        if (mode == "composite" || mode == "Composite")
            composite = true;
        layer = LuaOptLayerIndex(L, 3, -1);
    }
    else
    {
        layer = LuaOptLayerIndex(L, 2, -1);
    }

    const bool ok = composite ? b->canvas->CopySelectionToClipboardComposite()
                              : b->canvas->CopySelectionToClipboard(layer);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int l_canvas_cutSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    // cutSelection([layer])  (composite cut is not supported)
    const int layer = LuaOptLayerIndex(L, 2, -1);
    lua_pushboolean(L, b->canvas->CutSelectionToClipboard(layer) ? 1 : 0);
    return 1;
}

static int l_canvas_deleteSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int layer = LuaOptLayerIndex(L, 2, -1);
    lua_pushboolean(L, b->canvas->DeleteSelection(layer) ? 1 : 0);
    return 1;
}

static int l_canvas_pasteClipboard(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    // pasteClipboard(x, y, [layer], [mode], [transparentSpaces])
    int layer = -1;
    int idx = 4;
    if (lua_gettop(L) >= 4 && lua_isnumber(L, 4))
    {
        layer = (int)lua_tointeger(L, 4);
        idx = 5;
    }
    const AnsiCanvas::PasteMode mode = ParsePasteMode(L, idx, AnsiCanvas::PasteMode::Both);
    const bool transparent = (lua_gettop(L) >= (idx + 1)) ? (lua_toboolean(L, idx + 1) != 0) : false;
    lua_pushboolean(L, b->canvas->PasteClipboard(x, y, layer, mode, transparent) ? 1 : 0);
    return 1;
}

static int l_canvas_isMovingSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    lua_pushboolean(L, (b && b->canvas && b->canvas->IsMovingSelection()) ? 1 : 0);
    return 1;
}

static int l_canvas_beginMoveSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int grab_x = (int)luaL_checkinteger(L, 2);
    const int grab_y = (int)luaL_checkinteger(L, 3);
    const bool copy = (lua_gettop(L) >= 4) ? (lua_toboolean(L, 4) != 0) : false;
    const int layer = LuaOptLayerIndex(L, 5, -1);
    lua_pushboolean(L, b->canvas->BeginMoveSelection(grab_x, grab_y, copy, layer) ? 1 : 0);
    return 1;
}

static int l_canvas_updateMoveSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int x = (int)luaL_checkinteger(L, 2);
    const int y = (int)luaL_checkinteger(L, 3);
    b->canvas->UpdateMoveSelection(x, y);
    return 0;
}

static int l_canvas_commitMoveSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int layer = LuaOptLayerIndex(L, 2, -1);
    lua_pushboolean(L, b->canvas->CommitMoveSelection(layer) ? 1 : 0);
    return 1;
}

static int l_canvas_cancelMoveSelection(lua_State* L)
{
    CanvasBinding* b = CheckCanvas(L, 1);
    if (!b || !b->canvas)
        return luaL_error(L, "Invalid canvas binding");
    const int layer = LuaOptLayerIndex(L, 2, -1);
    lua_pushboolean(L, b->canvas->CancelMoveSelection(layer) ? 1 : 0);
    return 1;
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

    // Also return optional fg/bg as xterm-256 indices (nil means "unset").
    // We intentionally return multiple values for backward compatibility:
    //   local ch = layer:get(x,y)        -- old scripts still work (Lua keeps first)
    //   local ch, fg, bg = layer:get(x,y)
    AnsiCanvas::Color32 fg32 = 0;
    AnsiCanvas::Color32 bg32 = 0;
    int fg_idx = -1;
    int bg_idx = -1;
    if (b->canvas->GetLayerCellColors(b->layer_index, y, x, fg32, bg32))
    {
        fg_idx = Color32ToXtermIndex(fg32);
        bg_idx = Color32ToXtermIndex(bg32);
    }

    if (fg_idx >= 0) lua_pushinteger(L, fg_idx);
    else lua_pushnil(L);
    if (bg_idx >= 0) lua_pushinteger(L, bg_idx);
    else lua_pushnil(L);
    // Also return cp as an integer (safe additive API).
    lua_pushinteger(L, (lua_Integer)cp);
    return 4;
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

static int l_canvas_gc(lua_State* L)
{
    CanvasBinding* b = static_cast<CanvasBinding*>(luaL_checkudata(L, 1, "AnsiCanvas"));
    if (b)
        b->canvas = nullptr;
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

static void EnsureCanvasMetatable(lua_State* L)
{
    if (luaL_newmetatable(L, "AnsiCanvas"))
    {
        lua_pushcfunction(L, l_canvas_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L); // __index
        lua_pushcfunction(L, l_canvas_hasSelection);      lua_setfield(L, -2, "hasSelection");
        lua_pushcfunction(L, l_canvas_getSelection);      lua_setfield(L, -2, "getSelection");
        lua_pushcfunction(L, l_canvas_getCell);           lua_setfield(L, -2, "getCell");
        lua_pushcfunction(L, l_canvas_setSelection);      lua_setfield(L, -2, "setSelection");
        lua_pushcfunction(L, l_canvas_clearSelection);    lua_setfield(L, -2, "clearSelection");
        lua_pushcfunction(L, l_canvas_selectionContains); lua_setfield(L, -2, "selectionContains");
        lua_pushcfunction(L, l_canvas_clipboardHas);      lua_setfield(L, -2, "clipboardHas");
        lua_pushcfunction(L, l_canvas_clipboardSize);     lua_setfield(L, -2, "clipboardSize");
        lua_pushcfunction(L, l_canvas_copySelection);     lua_setfield(L, -2, "copySelection");
        lua_pushcfunction(L, l_canvas_cutSelection);      lua_setfield(L, -2, "cutSelection");
        lua_pushcfunction(L, l_canvas_deleteSelection);   lua_setfield(L, -2, "deleteSelection");
        lua_pushcfunction(L, l_canvas_pasteClipboard);    lua_setfield(L, -2, "pasteClipboard");
        lua_pushcfunction(L, l_canvas_isMovingSelection); lua_setfield(L, -2, "isMovingSelection");
        lua_pushcfunction(L, l_canvas_beginMoveSelection); lua_setfield(L, -2, "beginMoveSelection");
        lua_pushcfunction(L, l_canvas_updateMoveSelection); lua_setfield(L, -2, "updateMoveSelection");
        lua_pushcfunction(L, l_canvas_commitMoveSelection); lua_setfield(L, -2, "commitMoveSelection");
        lua_pushcfunction(L, l_canvas_cancelMoveSelection); lua_setfield(L, -2, "cancelMoveSelection");
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

static CanvasBinding* PushCanvasObject(lua_State* L, AnsiCanvas* canvas)
{
    EnsureCanvasMetatable(L);
    auto* b = static_cast<CanvasBinding*>(lua_newuserdata(L, sizeof(CanvasBinding)));
    b->canvas = canvas;
    luaL_setmetatable(L, "AnsiCanvas");
    return b;
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

static bool LuaIsStringField(lua_State* L, int idx, const char* field, std::string& out)
{
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, field);
    if (!lua_isstring(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    out.assign(s ? s : "", s ? (s + len) : "");
    lua_pop(L, 1);
    return true;
}

static bool LuaIsNumberField(lua_State* L, int idx, const char* field, lua_Number& out)
{
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, field);
    if (!lua_isnumber(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return true;
}

static bool LuaIsBoolField(lua_State* L, int idx, const char* field, bool& out)
{
    if (!lua_istable(L, idx))
        return false;
    lua_getfield(L, idx, field);
    if (!lua_isboolean(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return true;
}

// LuaJIT (Lua 5.1) compatibility: lua_rawlen exists only in Lua 5.2+.
static size_t LuaRawLen(lua_State* L, int idx)
{
    return (size_t)lua_objlen(L, idx);
}

static bool ReadScriptParams(lua_State* L,
                             std::vector<AnslParamSpec>& out_specs,
                             std::unordered_map<std::string, AnslParamValue>& out_defaults,
                             std::string& error)
{
    out_specs.clear();
    out_defaults.clear();
    error.clear();

    lua_getglobal(L, "settings");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return true; // no settings: ok
    }

    lua_getfield(L, -1, "params");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 2); // params + settings
        return true; // no params: ok
    }

    // Iterate settings.params = { key = {type=..., default=..., ...}, ... }
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
        // stack: settings, params, key, val
        if (!lua_isstring(L, -2) || !lua_istable(L, -1))
        {
            lua_pop(L, 1); // pop val, keep key
            continue;
        }

        size_t klen = 0;
        const char* k = lua_tolstring(L, -2, &klen);
        const std::string key = std::string(k ? k : "", k ? (k + klen) : (k ? k : ""));
        if (key.empty())
        {
            lua_pop(L, 1);
            continue;
        }

        AnslParamSpec spec;
        spec.key = key;

        // label (optional)
        (void)LuaIsStringField(L, -1, "label", spec.label);
        // layout hints (optional)
        (void)LuaIsBoolField(L, -1, "sameLine", spec.same_line);
        // ordering (optional)
        lua_Number ordn = 0;
        if (LuaIsNumberField(L, -1, "order", ordn))
        {
            spec.order = (int)std::llround(ordn);
            spec.order_set = true;
        }

        // type (required)
        std::string type_s;
        if (!LuaIsStringField(L, -1, "type", type_s))
        {
            error = "settings.params." + key + ": missing string field 'type'";
            lua_pop(L, 2); // val + key
            lua_pop(L, 2); // params + settings
            return false;
        }

        auto type_lower = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        type_s = type_lower(type_s);

        AnslParamValue def;
        if (type_s == "bool" || type_s == "boolean")
        {
            spec.type = AnslParamType::Bool;
            def.type = AnslParamType::Bool;
            bool dv = false;
            (void)LuaIsBoolField(L, -1, "default", dv);
            def.b = dv;
        }
        else if (type_s == "button")
        {
            // Edge-triggered button. Default is always false; host sets it true for one frame on click.
            spec.type = AnslParamType::Button;
            def.type = AnslParamType::Button;
            def.b = false;
        }
        else if (type_s == "int" || type_s == "integer")
        {
            spec.type = AnslParamType::Int;
            def.type = AnslParamType::Int;

            lua_Number n = 0;
            if (LuaIsNumberField(L, -1, "min", n)) spec.int_min = (int)std::llround(n);
            if (LuaIsNumberField(L, -1, "max", n)) spec.int_max = (int)std::llround(n);
            if (LuaIsNumberField(L, -1, "step", n)) spec.int_step = std::max(1, (int)std::llround(n));

            int dv = 0;
            if (LuaIsNumberField(L, -1, "default", n)) dv = (int)std::llround(n);
            def.i = dv;
        }
        else if (type_s == "float" || type_s == "number")
        {
            spec.type = AnslParamType::Float;
            def.type = AnslParamType::Float;

            lua_Number n = 0;
            if (LuaIsNumberField(L, -1, "min", n)) spec.float_min = (float)n;
            if (LuaIsNumberField(L, -1, "max", n)) spec.float_max = (float)n;
            if (LuaIsNumberField(L, -1, "step", n)) spec.float_step = (float)n;

            float dv = 0.0f;
            if (LuaIsNumberField(L, -1, "default", n)) dv = (float)n;
            def.f = dv;
        }
        else if (type_s == "enum")
        {
            spec.type = AnslParamType::Enum;
            def.type = AnslParamType::Enum;

            lua_getfield(L, -1, "items");
            if (!lua_istable(L, -1))
            {
                lua_pop(L, 1);
                error = "settings.params." + key + ": enum requires table field 'items'";
                lua_pop(L, 2); // val + key
                lua_pop(L, 2); // params + settings
                return false;
            }
            const int items_idx = lua_gettop(L);
            const int n_items = (int)LuaRawLen(L, items_idx);
            spec.enum_items.clear();
            spec.enum_items.reserve(std::max(0, n_items));
            for (int i = 1; i <= n_items; ++i)
            {
                lua_rawgeti(L, items_idx, i);
                if (lua_isstring(L, -1))
                {
                    size_t len = 0;
                    const char* s = lua_tolstring(L, -1, &len);
                    if (s && len > 0)
                        spec.enum_items.emplace_back(s, s + len);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // items

            if (spec.enum_items.empty())
            {
                error = "settings.params." + key + ": enum 'items' must contain at least one string";
                lua_pop(L, 2); // val + key
                lua_pop(L, 2); // params + settings
                return false;
            }

            std::string dv;
            if (LuaIsStringField(L, -1, "default", dv) && !dv.empty())
                def.s = dv;
            else
                def.s = spec.enum_items.front();
        }
        else
        {
            error = "settings.params." + key + ": unknown type '" + type_s + "'";
            lua_pop(L, 2); // val + key
            lua_pop(L, 2); // params + settings
            return false;
        }

        out_defaults[key] = def;
        out_specs.push_back(std::move(spec));

        lua_pop(L, 1); // pop val, keep key for lua_next
    }

    // stable order so UI doesn't jump around
    std::sort(out_specs.begin(), out_specs.end(), [](const AnslParamSpec& a, const AnslParamSpec& b) {
        // Prefer explicit ordering when provided.
        if (a.order_set || b.order_set)
        {
            if (a.order != b.order) return a.order < b.order;
        }
        if (a.label != b.label) return a.label < b.label;
        return a.key < b.key;
    });

    lua_pop(L, 2); // params + settings
    return true;
}
} // namespace

struct AnslScriptEngine::Impl
{
    lua_State* L = nullptr;
    int render_ref = LUA_NOREF;
    int ctx_ref = LUA_NOREF; // reusable ctx table (with nested metrics/cursor/p tables)
    int params_ref = LUA_NOREF; // reusable ctx.params table
    std::string last_source;
    bool initialized = false;
    AnslScriptSettings settings;

    std::vector<AnslParamSpec> params;
    std::unordered_map<std::string, AnslParamValue> param_values;
    std::unordered_map<std::string, AnslParamValue> param_defaults;

    // For ctx.actions: we nil out previously-set keys each frame so the table only
    // contains edge-triggered pressed actions for the current frame.
    std::vector<std::string> prev_actions;

    // Assets root (used for native helpers like font discovery).
    std::string assets_dir;
    // Text-art font registry backing ansl.font.* (owned by this engine instance).
    std::unique_ptr<textmode_font::Registry> font_registry;
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
        if (impl_->params_ref != LUA_NOREF)
            luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->params_ref);
        lua_close(impl_->L);
    }

    delete impl_;
    impl_ = nullptr;
}

bool AnslScriptEngine::Init(const std::string& assets_dir, std::string& error)
{
    if (!impl_)
        return false;
    if (impl_->initialized)
        return true;

    impl_->assets_dir = assets_dir;

    impl_->L = luaL_newstate();
    if (!impl_->L)
    {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(impl_->L);

    // Register per-engine native data before luaopen_ansl runs.
    // luaopen_ansl can retrieve this from LUA_REGISTRYINDEX.
    impl_->font_registry = std::make_unique<textmode_font::Registry>();
    {
        std::string ferr;
        (void)impl_->font_registry->Scan(assets_dir, ferr);
        // Non-fatal: ansl.font.list() will be empty if scan failed.
    }
    lua_pushlightuserdata(impl_->L, impl_->font_registry.get());
    lua_setfield(impl_->L, LUA_REGISTRYINDEX, "phosphor.textmode_font_registry");

    // Register native ansl library as require('ansl') and global `ansl`.
    if (!EnsureAnslModule(impl_->L, error))
        return false;

    // Pre-create a reusable ctx table to avoid per-frame allocations/GC churn.
    // ctx = {
    //   cols, rows, frame, time, fg, bg,
    //   focused, phase,
    //   keys={...}, typed={...},
    //   mods={ctrl,shift,alt,super},
    //   hotkeys={copy,cut,paste,selectAll,cancel,deleteSelection},
    //   actions={ ["edit.copy"]=true, ... },  -- pressed this frame
    //   out={ {type="palette.set", ...}, ... }, -- tool commands (cleared each frame)
    //   metrics={aspect=...},
    //   caret={x,y},
    //   cursor={valid,x,y,left,right,p={x,y,left,right}},
    //   canvas=<AnsiCanvas userdata>
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
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "c");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "v");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "x");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "a");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "escape");
    lua_setfield(impl_->L, -2, "keys"); // ctx.keys = keys

    // mods table (reused)
    lua_newtable(impl_->L); // mods
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "ctrl");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "shift");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "alt");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "super");
    lua_setfield(impl_->L, -2, "mods"); // ctx.mods = mods

    // hotkeys table (reused): ctx.hotkeys = { copy=false, ... }
    lua_newtable(impl_->L); // hotkeys
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "copy");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "cut");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "paste");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "selectAll");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "cancel");
    lua_pushboolean(impl_->L, 0); lua_setfield(impl_->L, -2, "deleteSelection");
    lua_setfield(impl_->L, -2, "hotkeys"); // ctx.hotkeys = hotkeys

    // actions table (reused): ctx.actions = {}
    lua_newtable(impl_->L);
    lua_setfield(impl_->L, -2, "actions");

    // params table (reused): ctx.params = {}
    lua_newtable(impl_->L); // params
    impl_->params_ref = luaL_ref(impl_->L, LUA_REGISTRYINDEX); // pops params
    lua_rawgeti(impl_->L, LUA_REGISTRYINDEX, impl_->params_ref);
    lua_setfield(impl_->L, -2, "params"); // ctx.params = params

    // Tool command queue (reused): ctx.out = { {type=...}, ... }
    lua_newtable(impl_->L);
    lua_setfield(impl_->L, -2, "out");

    // Tool brush defaults
    lua_pushliteral(impl_->L, " ");
    lua_setfield(impl_->L, -2, "brush");
    lua_pushinteger(impl_->L, 32);
    lua_setfield(impl_->L, -2, "brushCp");

    // Canvas userdata (reused): ctx.canvas
    (void)PushCanvasObject(impl_->L, nullptr);
    lua_setfield(impl_->L, -2, "canvas");

    impl_->ctx_ref = luaL_ref(impl_->L, LUA_REGISTRYINDEX);

    impl_->initialized = true;
    impl_->settings = AnslScriptSettings{};
    impl_->params.clear();
    impl_->param_values.clear();
    impl_->param_defaults.clear();
    impl_->prev_actions.clear();
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
    impl_->params.clear();
    impl_->param_defaults.clear();
    if (impl_->render_ref != LUA_NOREF)
    {
        luaL_unref(impl_->L, LUA_REGISTRYINDEX, impl_->render_ref);
        impl_->render_ref = LUA_NOREF;
    }

    // Evaluate user source as a chunk. Users should define global:
    // - function render(ctx, layer) ... end
    // or classic: function main(coord, context, cursor, buffer) ... end (host wraps)
    lua_settop(impl_->L, 0);

    // IMPORTANT: The Lua state persists across compiles. If a new script does not define
    // `settings` (or `main`/`render`), we must not accidentally keep using the old ones.
    // Clear script-owned globals before evaluating the new chunk.
    auto clear_global = [&](const char* name) {
        lua_pushnil(impl_->L);
        lua_setglobal(impl_->L, name);
    };
    clear_global("settings");
    clear_global("render");
    clear_global("main");
    clear_global("pre");
    clear_global("post");

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

    // Read settings.params (optional) -> host-managed params -> ctx.params.
    {
        std::vector<AnslParamSpec> specs;
        std::unordered_map<std::string, AnslParamValue> defaults;
        std::string perr;
        if (!ReadScriptParams(impl_->L, specs, defaults, perr))
        {
            error = perr;
            return false;
        }

        // Preserve compatible previous values when possible; otherwise use defaults.
        std::unordered_map<std::string, AnslParamValue> new_values;
        for (const auto& s : specs)
        {
            auto def_it = defaults.find(s.key);
            if (def_it == defaults.end())
                continue;
            const AnslParamValue& def = def_it->second;

            auto old_it = impl_->param_values.find(s.key);
            if (old_it != impl_->param_values.end() && old_it->second.type == def.type)
                new_values[s.key] = old_it->second;
            else
                new_values[s.key] = def;
        }

        impl_->params = std::move(specs);
        impl_->param_defaults = std::move(defaults);
        impl_->param_values = std::move(new_values);
    }

    error.clear();
    return true;
}

bool AnslScriptEngine::RunFrame(AnsiCanvas& canvas,
                               int layer_index,
                               const AnslFrameContext& frame_ctx,
                               const ToolCommandSink& tool_cmds,
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

    // Update ctx.canvas binding to point at the current canvas.
    lua_getfield(L, -1, "canvas");
    if (lua_isuserdata(L, -1))
    {
        CanvasBinding* cb = static_cast<CanvasBinding*>(luaL_checkudata(L, -1, "AnsiCanvas"));
        if (cb)
            cb->canvas = &canvas;
    }
    lua_pop(L, 1); // canvas

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

    // Brush selection for tools
    if (!frame_ctx.brush_utf8.empty())
        lua_pushlstring(L, frame_ctx.brush_utf8.data(), frame_ctx.brush_utf8.size());
    else
        lua_pushliteral(L, " ");
    lua_setfield(L, -2, "brush");
    lua_pushinteger(L, (lua_Integer)frame_ctx.brush_cp);
    lua_setfield(L, -2, "brushCp");

    // params table: sync host values into ctx.params.* each frame
    if (impl_->params_ref != LUA_NOREF && !impl_->params.empty())
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->params_ref); // params
        for (const auto& s : impl_->params)
        {
            auto it = impl_->param_values.find(s.key);
            if (it == impl_->param_values.end())
                continue;
            AnslParamValue& v = it->second;
            switch (v.type)
            {
                case AnslParamType::Bool:  lua_pushboolean(L, v.b ? 1 : 0); break;
                case AnslParamType::Int:   lua_pushinteger(L, (lua_Integer)v.i); break;
                case AnslParamType::Float: lua_pushnumber(L, (lua_Number)v.f); break;
                case AnslParamType::Enum:  lua_pushlstring(L, v.s.c_str(), v.s.size()); break;
                case AnslParamType::Button:
                {
                    lua_pushboolean(L, v.b ? 1 : 0);
                    // Button is edge-triggered: reset immediately after exposing it to ctx.params.
                    v.b = false;
                } break;
            }
            lua_setfield(L, -2, s.key.c_str());
        }
        lua_pop(L, 1); // params
    }

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
        lua_pushboolean(L, frame_ctx.key_c);         lua_setfield(L, -2, "c");
        lua_pushboolean(L, frame_ctx.key_v);         lua_setfield(L, -2, "v");
        lua_pushboolean(L, frame_ctx.key_x);         lua_setfield(L, -2, "x");
        lua_pushboolean(L, frame_ctx.key_a);         lua_setfield(L, -2, "a");
        lua_pushboolean(L, frame_ctx.key_escape);    lua_setfield(L, -2, "escape");
    }
    lua_pop(L, 1); // keys

    // mods table
    lua_getfield(L, -1, "mods");
    if (lua_istable(L, -1))
    {
        lua_pushboolean(L, frame_ctx.mod_ctrl ? 1 : 0);   lua_setfield(L, -2, "ctrl");
        lua_pushboolean(L, frame_ctx.mod_shift ? 1 : 0);  lua_setfield(L, -2, "shift");
        lua_pushboolean(L, frame_ctx.mod_alt ? 1 : 0);    lua_setfield(L, -2, "alt");
        lua_pushboolean(L, frame_ctx.mod_super ? 1 : 0);  lua_setfield(L, -2, "super");
    }
    lua_pop(L, 1); // mods

    // hotkeys table
    lua_getfield(L, -1, "hotkeys");
    if (lua_istable(L, -1))
    {
        lua_pushboolean(L, frame_ctx.hotkeys.copy ? 1 : 0);            lua_setfield(L, -2, "copy");
        lua_pushboolean(L, frame_ctx.hotkeys.cut ? 1 : 0);             lua_setfield(L, -2, "cut");
        lua_pushboolean(L, frame_ctx.hotkeys.paste ? 1 : 0);           lua_setfield(L, -2, "paste");
        lua_pushboolean(L, frame_ctx.hotkeys.selectAll ? 1 : 0);       lua_setfield(L, -2, "selectAll");
        lua_pushboolean(L, frame_ctx.hotkeys.cancel ? 1 : 0);          lua_setfield(L, -2, "cancel");
        lua_pushboolean(L, frame_ctx.hotkeys.deleteSelection ? 1 : 0); lua_setfield(L, -2, "deleteSelection");
    }
    lua_pop(L, 1); // hotkeys

    // actions table: clear keys set last frame, then set currently-pressed actions to true.
    lua_getfield(L, -1, "actions");
    if (lua_istable(L, -1))
    {
        // Clear previous pressed keys.
        for (const std::string& k : impl_->prev_actions)
        {
            lua_pushlstring(L, k.data(), k.size());
            lua_pushnil(L);
            lua_settable(L, -3);
        }
        impl_->prev_actions.clear();

        if (frame_ctx.actions_pressed)
        {
            for (const std::string& id : *frame_ctx.actions_pressed)
            {
                lua_pushlstring(L, id.data(), id.size());
                lua_pushboolean(L, 1);
                lua_settable(L, -3);
                impl_->prev_actions.push_back(id);
            }
        }
    }
    lua_pop(L, 1); // actions

    // Tool command queue: clear array entries each frame.
    if (tool_cmds.allow_tool_commands)
    {
        lua_getfield(L, -1, "out");
        if (lua_istable(L, -1))
        {
            const int n = LuaArrayLen(L, -1);
            for (int i = 1; i <= n; ++i)
            {
                lua_pushnil(L);
                lua_rawseti(L, -2, i);
            }
        }
        lua_pop(L, 1); // out
    }

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
        lua_pushinteger(L, frame_ctx.cursor_half_y); lua_setfield(L, -2, "half_y");
        lua_pushboolean(L, frame_ctx.cursor_left_down); lua_setfield(L, -2, "left");
        lua_pushboolean(L, frame_ctx.cursor_right_down); lua_setfield(L, -2, "right");

        lua_getfield(L, -1, "p");
        if (lua_istable(L, -1))
        {
            lua_pushinteger(L, frame_ctx.cursor_px); lua_setfield(L, -2, "x");
            lua_pushinteger(L, frame_ctx.cursor_py); lua_setfield(L, -2, "y");
            lua_pushinteger(L, frame_ctx.cursor_phalf_y); lua_setfield(L, -2, "half_y");
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

    // Tool command support: parse ctx.out into host commands.
    if (tool_cmds.allow_tool_commands && tool_cmds.out_commands)
    {
        tool_cmds.out_commands->clear();

        lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->ctx_ref); // ctx
        lua_getfield(L, -1, "out"); // ctx, out
        if (lua_istable(L, -1))
        {
            const int n = LuaArrayLen(L, -1);
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i); // elem
                if (!lua_istable(L, -1))
                {
                    lua_pop(L, 1);
                    continue;
                }

                lua_getfield(L, -1, "type");
                const std::string type = lua_isstring(L, -1) ? LuaToString(L, -1) : std::string{};
                lua_pop(L, 1);
                if (type.empty())
                {
                    lua_pop(L, 1);
                    continue;
                }

                ToolCommand cmd;
                if (type == "palette.set")
                {
                    cmd.type = ToolCommand::Type::PaletteSet;
                    lua_getfield(L, -1, "fg");
                    if (lua_isnumber(L, -1))
                    {
                        cmd.has_fg = true;
                        cmd.fg = (int)lua_tointeger(L, -1);
                    }
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "bg");
                    if (lua_isnumber(L, -1))
                    {
                        cmd.has_bg = true;
                        cmd.bg = (int)lua_tointeger(L, -1);
                    }
                    lua_pop(L, 1);
                    tool_cmds.out_commands->push_back(std::move(cmd));
                }
                else if (type == "brush.set")
                {
                    cmd.type = ToolCommand::Type::BrushSet;
                    lua_getfield(L, -1, "cp");
                    if (lua_isnumber(L, -1))
                    {
                        lua_Integer v = lua_tointeger(L, -1);
                        if (v < 0) v = 0;
                        cmd.brush_cp = (uint32_t)v;
                        tool_cmds.out_commands->push_back(std::move(cmd));
                    }
                    lua_pop(L, 1);
                }
                else if (type == "tool.activate_prev")
                {
                    cmd.type = ToolCommand::Type::ToolActivatePrev;
                    tool_cmds.out_commands->push_back(std::move(cmd));
                }
                else if (type == "tool.activate")
                {
                    cmd.type = ToolCommand::Type::ToolActivate;
                    lua_getfield(L, -1, "id");
                    if (lua_isstring(L, -1))
                        cmd.tool_id = LuaToString(L, -1);
                    lua_pop(L, 1);
                    if (!cmd.tool_id.empty())
                        tool_cmds.out_commands->push_back(std::move(cmd));
                }
                else if (type == "canvas.crop_to_selection")
                {
                    cmd.type = ToolCommand::Type::CanvasCropToSelection;
                    tool_cmds.out_commands->push_back(std::move(cmd));
                }

                lua_pop(L, 1); // elem
            }
        }
        lua_pop(L, 2); // out, ctx
    }

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

bool AnslScriptEngine::HasParams() const
{
    return impl_ && impl_->initialized && !impl_->params.empty();
}

const std::vector<AnslParamSpec>& AnslScriptEngine::GetParamSpecs() const
{
    static const std::vector<AnslParamSpec> empty;
    if (!impl_ || !impl_->initialized)
        return empty;
    return impl_->params;
}

bool AnslScriptEngine::GetParamBool(const std::string& key, bool& out) const
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Bool)
        return false;
    out = it->second.b;
    return true;
}

bool AnslScriptEngine::GetParamInt(const std::string& key, int& out) const
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Int)
        return false;
    out = it->second.i;
    return true;
}

bool AnslScriptEngine::GetParamFloat(const std::string& key, float& out) const
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Float)
        return false;
    out = it->second.f;
    return true;
}

bool AnslScriptEngine::GetParamEnum(const std::string& key, std::string& out) const
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Enum)
        return false;
    out = it->second.s;
    return true;
}

bool AnslScriptEngine::SetParamBool(const std::string& key, bool v)
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Bool)
        return false;
    it->second.b = v;
    return true;
}

bool AnslScriptEngine::SetParamInt(const std::string& key, int v)
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Int)
        return false;
    it->second.i = v;
    return true;
}

bool AnslScriptEngine::SetParamFloat(const std::string& key, float v)
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Float)
        return false;
    it->second.f = v;
    return true;
}

bool AnslScriptEngine::SetParamEnum(const std::string& key, std::string v)
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Enum)
        return false;
    it->second.s = std::move(v);
    return true;
}

bool AnslScriptEngine::FireParamButton(const std::string& key)
{
    if (!impl_ || !impl_->initialized)
        return false;
    auto it = impl_->param_values.find(key);
    if (it == impl_->param_values.end() || it->second.type != AnslParamType::Button)
        return false;
    it->second.b = true;
    return true;
}

void AnslScriptEngine::ResetParamsToDefaults()
{
    if (!impl_ || !impl_->initialized)
        return;
    impl_->param_values = impl_->param_defaults;
}


