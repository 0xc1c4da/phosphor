#include "ui/tool_palette.h"

#include "core/i18n.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// Compute tight glyph bounds for a UTF-8 string rendered with `font` at `font_size`.
// This is used for "optical centering" of icon glyphs (especially emoji), where
// line-height-based centering can look visibly off.
static bool CalcTightTextBounds(ImFont* font, float font_size, const char* text_begin, const char* text_end,
                                ImVec2& out_min, ImVec2& out_max)
{
    out_min = ImVec2(FLT_MAX, FLT_MAX);
    out_max = ImVec2(-FLT_MAX, -FLT_MAX);
    if (!font || font_size <= 0.0f || !text_begin || text_begin == text_end)
        return false;

    if (!text_end)
        text_end = text_begin + strlen(text_begin);

    // ImGui 1.92+ moved glyph lookup/metrics to ImFontBaked (per-size baked data).
    // Older ImGui versions expose glyphs directly on ImFont with a fixed FontSize.
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
    ImFontBaked* baked = font->GetFontBaked(font_size);
    if (!baked)
        return false;
#else
    // Scale from font->FontSize (built size) to requested pixel size.
    const float scale = font_size / font->FontSize;
#endif

    float x = 0.0f;
    float y = 0.0f;
    bool any = false;
    const char* s = text_begin;
    while (s < text_end && *s)
    {
        unsigned int c = 0;
        const int bytes = ImTextCharFromUtf8(&c, s, text_end);
        if (bytes <= 0)
            break;
        s += bytes;

        // Newlines are not expected for icons, but handle gracefully.
        if (c == '\n')
        {
            x = 0.0f;
            y += font_size;
            continue;
        }

#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
        const ImFontGlyph* glyph = baked->FindGlyphNoFallback((ImWchar)c);
        if (!glyph)
            continue;

        // In 1.92+, glyph metrics are already in pixels for the baked size.
        out_min.x = ImMin(out_min.x, x + glyph->X0);
        out_min.y = ImMin(out_min.y, y + glyph->Y0);
        out_max.x = ImMax(out_max.x, x + glyph->X1);
        out_max.y = ImMax(out_max.y, y + glyph->Y1);

        x += glyph->AdvanceX;
#else
        const ImFontGlyph* glyph = font->FindGlyph((ImWchar)c);
        if (!glyph)
            continue;

        // glyph->X0..Y1 are in font units (at FontSize); scale into pixels.
        out_min.x = ImMin(out_min.x, x + glyph->X0 * scale);
        out_min.y = ImMin(out_min.y, y + glyph->Y0 * scale);
        out_max.x = ImMax(out_max.x, x + glyph->X1 * scale);
        out_max.y = ImMax(out_max.y, y + glyph->Y1 * scale);

        x += glyph->AdvanceX * scale;
#endif
        any = true;
    }

    if (!any)
        return false;

    // In case all glyphs were empty/zero-area, still consider it "no bounds".
    if (out_min.x > out_max.x || out_min.y > out_max.y)
        return false;

    return true;
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

static std::string ReadFileToString(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static std::string BasenameNoExt(const std::string& path)
{
    fs::path p(path);
    std::string name = p.filename().string();
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.erase(dot);
    return name;
}

static void LuaReadStringArrayField(lua_State* L, int table_idx, const char* key, std::vector<std::string>& out)
{
    out.clear();
    lua_getfield(L, table_idx, key);
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    const int n = LuaArrayLen(L, -1);
    out.reserve((size_t)std::max(0, n));
    for (int i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, -1, i);
        if (lua_isstring(L, -1))
        {
            const char* s = lua_tostring(L, -1);
            if (s && *s)
                out.emplace_back(s);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // field table
}

static ToolSpec::HandleWhen ParseHandleWhen(std::string_view s)
{
    if (s == "inactive")
        return ToolSpec::HandleWhen::Inactive;
    return ToolSpec::HandleWhen::Active;
}

static void LuaReadHandlesField(lua_State* L, int table_idx, std::vector<ToolSpec::HandleRule>& out)
{
    out.clear();
    lua_getfield(L, table_idx, "handles");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    const int n = LuaArrayLen(L, -1);
    out.reserve((size_t)std::max(0, n));
    for (int i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            continue;
        }

        ToolSpec::HandleRule r;

        lua_getfield(L, -1, "action");
        if (lua_isstring(L, -1))
            r.action = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "when");
        if (lua_isstring(L, -1))
            r.when = ParseHandleWhen(lua_tostring(L, -1));
        lua_pop(L, 1);

        if (!r.action.empty())
            out.push_back(std::move(r));

        lua_pop(L, 1); // rule table
    }
    lua_pop(L, 1); // handles
}

bool ToolPalette::ParseToolSettingsFromLuaFile(const std::string& path, ToolSpec& out, std::string& error)
{
    out = ToolSpec{};
    out.id = BasenameNoExt(path);
    out.path = path;
    out.icon = "?";
    out.label = BasenameNoExt(path);
    out.actions.clear();
    out.handles.clear();

    const std::string src = ReadFileToString(path);
    if (src.empty())
    {
        error = "Failed to read tool file: " + path;
        return false;
    }

    lua_State* L = luaL_newstate();
    if (!L)
    {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(L);

    if (luaL_loadbuffer(L, src.c_str(), src.size(), path.c_str()) != LUA_OK)
    {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua load error";
        lua_close(L);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
    {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua runtime error";
        lua_close(L);
        return false;
    }

    lua_getglobal(L, "settings");
    if (lua_istable(L, -1))
    {
        auto get_string_field = [&](const char* key, std::string& dst) {
            lua_getfield(L, -1, key);
            if (lua_isstring(L, -1))
                dst = lua_tostring(L, -1);
            lua_pop(L, 1);
        };

        // Optional stable id.
        get_string_field("id", out.id);
        if (out.id.empty())
            out.id = BasenameNoExt(path);

        lua_getfield(L, -1, "icon");
        if (lua_isstring(L, -1))
            out.icon = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "label");
        if (lua_isstring(L, -1))
            out.label = lua_tostring(L, -1);
        lua_pop(L, 1);

        // Optional routing hints:
        // - preferred: settings.handles = { {action=..., when="active"/"inactive"}, ... }
        // - back-compat: settings.claims / settings.fallbackClaims (string arrays)
        LuaReadHandlesField(L, -1, out.handles);
        if (out.handles.empty())
        {
            std::vector<std::string> claims;
            std::vector<std::string> fallback;
            LuaReadStringArrayField(L, -1, "claims", claims);
            LuaReadStringArrayField(L, -1, "fallbackClaims", fallback);
            out.handles.reserve(claims.size() + fallback.size());
            for (const std::string& s : claims)
                out.handles.push_back(ToolSpec::HandleRule{.action = s, .when = ToolSpec::HandleWhen::Active});
            for (const std::string& s : fallback)
                out.handles.push_back(ToolSpec::HandleRule{.action = s, .when = ToolSpec::HandleWhen::Inactive});
        }

        // Optional: settings.actions = { {id=..., title=..., category=..., description=..., bindings={...}}, ... }
        lua_getfield(L, -1, "actions");
        if (lua_istable(L, -1))
        {
            const int n = LuaArrayLen(L, -1);
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i);
                if (!lua_istable(L, -1))
                {
                    lua_pop(L, 1);
                    continue;
                }

                kb::Action a;
                get_string_field("id", a.id);
                if (a.id.empty())
                {
                    lua_pop(L, 1);
                    continue;
                }
                get_string_field("title", a.title);
                get_string_field("category", a.category);
                get_string_field("description", a.description);

                // Defaults
                if (a.title.empty()) a.title = a.id;
                if (a.category.empty()) a.category = "Tool";

                // Parse bindings:
                // - bindings = { "Ctrl+K", "Alt+B", ... } (strings)
                // - OR bindings = { { chord="Ctrl+K", context="editor", platform="any", enabled=true }, ... }
                lua_getfield(L, -1, "bindings");
                if (lua_istable(L, -1))
                {
                    const int bn = LuaArrayLen(L, -1);
                    for (int bi = 1; bi <= bn; ++bi)
                    {
                        lua_rawgeti(L, -1, bi);
                        kb::KeyBinding b;
                        b.enabled = true;
                        b.context = "editor";
                        b.platform = "any";

                        if (lua_isstring(L, -1))
                        {
                            b.chord = lua_tostring(L, -1);
                            if (!b.chord.empty())
                                a.bindings.push_back(std::move(b));
                            lua_pop(L, 1);
                            continue;
                        }

                        if (lua_istable(L, -1))
                        {
                            lua_getfield(L, -1, "enabled");
                            if (lua_isboolean(L, -1))
                                b.enabled = lua_toboolean(L, -1) != 0;
                            lua_pop(L, 1);

                            lua_getfield(L, -1, "chord");
                            if (lua_isstring(L, -1))
                                b.chord = lua_tostring(L, -1);
                            lua_pop(L, 1);

                            lua_getfield(L, -1, "context");
                            if (lua_isstring(L, -1))
                                b.context = lua_tostring(L, -1);
                            lua_pop(L, 1);

                            lua_getfield(L, -1, "platform");
                            if (lua_isstring(L, -1))
                                b.platform = lua_tostring(L, -1);
                            lua_pop(L, 1);

                            if (!b.chord.empty())
                                a.bindings.push_back(std::move(b));
                        }
                        lua_pop(L, 1); // binding (string/table/other)
                    }
                }
                lua_pop(L, 1); // bindings

                if (!a.bindings.empty())
                    out.actions.push_back(std::move(a));

                lua_pop(L, 1); // action table
            }
        }
        lua_pop(L, 1); // actions
    }
    lua_pop(L, 1); // settings

    lua_close(L);
    error.clear();
    return true;
}

bool ToolPalette::SetActiveToolById(const std::string& id)
{
    if (id.empty())
        return false;
    for (size_t i = 0; i < tools_.size(); ++i)
    {
        if (tools_[i].id == id)
        {
            const bool changed = (active_index_ != (int)i);
            active_index_ = (int)i;
            if (changed)
                active_changed_ = true;
            return changed;
        }
    }
    return false;
}

bool ToolPalette::LoadFromDirectory(const std::string& tools_dir, std::string& error)
{
    // Preserve current selection by path if possible.
    std::string prev_active_path;
    if (const ToolSpec* t = GetActiveTool())
        prev_active_path = t->path;

    tools_dir_ = tools_dir;
    tools_.clear();
    active_index_ = 0;
    active_changed_ = true; // reload should force recompile even if tool didn't change

    fs::path dir(tools_dir);
    if (!fs::exists(dir) || !fs::is_directory(dir))
    {
        error = "Tools dir not found: " + tools_dir;
        return false;
    }

    std::vector<ToolSpec> found;
    std::string last_err;
    try
    {
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            fs::path p = entry.path();
            if (p.extension() != ".lua")
                continue;

            ToolSpec spec;
            std::string perr;
            if (ParseToolSettingsFromLuaFile(p.string(), spec, perr))
                found.push_back(std::move(spec));
            else
                last_err = perr;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    // Stable ordering by id then path (so UI doesn't jump around).
    std::sort(found.begin(), found.end(), [](const ToolSpec& a, const ToolSpec& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.path < b.path;
    });

    tools_ = std::move(found);
    if (tools_.empty())
    {
        error = last_err.empty() ? "No tools found in " + tools_dir : last_err;
        return false;
    }

    // Try keep previous selection.
    if (!prev_active_path.empty())
    {
        for (size_t i = 0; i < tools_.size(); ++i)
        {
            if (tools_[i].path == prev_active_path)
            {
                active_index_ = (int)i;
                prev_active_path.clear();
                break;
            }
        }
    }

    // Otherwise prefer default tool named "edit.lua" if present.
    if (!prev_active_path.empty())
    {
        for (size_t i = 0; i < tools_.size(); ++i)
        {
            if (fs::path(tools_[i].path).filename().string() == "edit.lua")
            {
                active_index_ = (int)i;
                break;
            }
        }
    }

    error.clear();
    return true;
}

const ToolSpec* ToolPalette::GetActiveTool() const
{
    if (active_index_ < 0 || active_index_ >= (int)tools_.size())
        return nullptr;
    return &tools_[(size_t)active_index_];
}

bool ToolPalette::TakeActiveToolChanged(std::string& out_path)
{
    if (!active_changed_)
        return false;
    active_changed_ = false;
    const ToolSpec* t = GetActiveTool();
    out_path = t ? t->path : std::string{};
    return !out_path.empty();
}

bool ToolPalette::TakeReloadRequested()
{
    if (!reload_requested_)
        return false;
    reload_requested_ = false;
    return true;
}

bool ToolPalette::SetActiveToolByPath(const std::string& path)
{
    if (path.empty() || tools_.empty())
        return false;
    for (int i = 0; i < (int)tools_.size(); ++i)
    {
        if (tools_[(size_t)i].path == path)
        {
            if (active_index_ != i)
            {
                active_index_ = i;
                active_changed_ = true;
                return true;
            }
            return false;
        }
    }
    return false;
}

bool ToolPalette::Render(const char* title, bool* p_open, SessionState* session, bool apply_placement_this_frame)
{
    bool changed_this_frame = false;
    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    const std::string win_title = PHOS_TR("menu.window.tool_palette") + "###" + std::string(title);
    if (!ImGui::Begin(win_title.c_str(), p_open, flags))
    {
        if (session)
        {
            // Even when collapsed, capture current pos/size/collapsed for persistence.
            CaptureImGuiWindowPlacement(*session, title);
        }
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return false;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, title);
        RenderImGuiWindowChromeMenu(session, title);
    }

    // Title-bar ⋮ popup for tool-palette actions/info.
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##toolpal_kebab", "\xE2\x8B\xAE", has_close, has_collapse, &kebab_min, &kebab_max))
            ImGui::OpenPopup("##toolpal_menu");

        if (ImGui::IsPopupOpen("##toolpal_menu"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(520.0f, 420.0f));
        if (ImGui::BeginPopup("##toolpal_menu"))
        {
            ImGui::TextUnformatted(PHOS_TR("tool_palette.titlebar_tools").c_str());
            ImGui::Separator();

            const std::string count = PHOS_TRF("tool_palette.count_fmt", phos::i18n::Arg::I64((long long)tools_.size()));
            ImGui::TextUnformatted(count.c_str());
            if (!tools_dir_.empty())
            {
                const std::string dir = PHOS_TRF("tool_palette.dir_fmt", phos::i18n::Arg::Str(tools_dir_));
                ImGui::TextUnformatted(dir.c_str());
            }

            const ToolSpec* t = GetActiveTool();
            if (t)
            {
                ImGui::Separator();
                const std::string active = PHOS_TRF("tool_palette.active_fmt", phos::i18n::Arg::Str(t->label));
                ImGui::TextUnformatted(active.c_str());
                ImGui::TextDisabled("%s", t->path.c_str());
            }

            ImGui::Separator();
            if (ImGui::Button(PHOS_TR("tool_palette.refresh_tools").c_str()))
            {
                reload_requested_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(PHOS_TR("common.close").c_str()))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }

    if (tools_.empty())
    {
        ImGui::TextUnformatted(PHOS_TR("tool_palette.no_tools_loaded").c_str());
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return changed_this_frame;
    }

    // Icon-only buttons in a grid.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int count = (int)tools_.size();

    // Fit-to-window sizing (similar to the colour palette adaptive grid):
    // choose cols that maximize square button size while fitting in width/height.
    int best_cols = 1;
    float best_size = 0.0f;
    if (count > 0 && avail.x > 1.0f)
    {
        for (int cols = 1; cols <= count; ++cols)
        {
            const float total_spacing_x = style.ItemSpacing.x * (cols - 1);
            const float width_limit = (avail.x - total_spacing_x) / (float)cols;
            if (width_limit <= 1.0f)
                break;

            const int rows = (count + cols - 1) / cols;
            float button_size = width_limit;
            if (avail.y > 1.0f)
            {
                const float total_spacing_y = style.ItemSpacing.y * (rows - 1);
                const float height_limit = (avail.y - total_spacing_y) / (float)rows;
                if (height_limit <= 1.0f)
                    continue;
                button_size = std::min(width_limit, height_limit);
            }

            if (button_size > best_size)
            {
                best_size = button_size;
                best_cols = cols;
            }
        }
    }

    if (best_size <= 0.0f)
    {
        best_cols = 1;
        best_size = style.FramePadding.y * 2.0f + 8.0f; // minimal fallback
    }

    const int cols = best_cols;
    const ImVec2 button_size(best_size, best_size);

    auto draw_centered_scaled_text = [&](const ImVec2& item_min, const ImVec2& item_max, const std::string& text) {
        if (text.empty())
            return;
        const ImGuiStyle& s = ImGui::GetStyle();
        const ImVec2 sz(item_max.x - item_min.x, item_max.y - item_min.y);
        const float max_w = std::max(1.0f, sz.x - s.FramePadding.x * 2.0f);
        const float max_h = std::max(1.0f, sz.y - s.FramePadding.y * 2.0f);

        ImFont* font = ImGui::GetFont();
        // Start large and shrink-to-fit, but center using tight glyph bounds for better optical centering.
        float font_size = std::max(1.0f, std::min(max_w, max_h) * 0.74f);

        ImVec2 bmin, bmax;
        bool have_bounds = CalcTightTextBounds(font, font_size, text.c_str(), nullptr, bmin, bmax);
        if (!have_bounds)
        {
            // Fallback: use line-height text size (better than nothing).
            const ImVec2 ts = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
            ImVec2 pos(item_min.x + (sz.x - ts.x) * 0.5f, item_min.y + (sz.y - ts.y) * 0.5f);
            pos.x = IM_FLOOR(pos.x + 0.5f);
            pos.y = IM_FLOOR(pos.y + 0.5f);
            ImGui::GetWindowDrawList()->AddText(font, font_size, pos,
                                               ImGui::GetColorU32(ImGuiCol_Text),
                                               text.c_str());
            return;
        }

        ImVec2 bsz(bmax.x - bmin.x, bmax.y - bmin.y);
        if (bsz.x > max_w || bsz.y > max_h)
        {
            const float sx = max_w / std::max(1.0f, bsz.x);
            const float sy = max_h / std::max(1.0f, bsz.y);
            font_size *= std::min(sx, sy);
            have_bounds = CalcTightTextBounds(font, font_size, text.c_str(), nullptr, bmin, bmax);
            if (!have_bounds)
                return;
            bsz = ImVec2(bmax.x - bmin.x, bmax.y - bmin.y);
        }

        // Center *bounds* within the full button rect. bmin can be negative for some glyphs,
        // so shift by -bmin to align the tight bbox to (0,0) before centering.
        ImVec2 pos(item_min.x + (sz.x - bsz.x) * 0.5f - bmin.x,
                   item_min.y + (sz.y - bsz.y) * 0.5f - bmin.y);
        pos.x = IM_FLOOR(pos.x + 0.5f);
        pos.y = IM_FLOOR(pos.y + 0.5f);
        ImGui::GetWindowDrawList()->AddText(font, font_size, pos,
                                           ImGui::GetColorU32(ImGuiCol_Text),
                                           text.c_str());
    };

    for (int i = 0; i < (int)tools_.size(); ++i)
    {
        if (i % cols != 0)
            ImGui::SameLine();
        ImGui::PushID(i);
        const bool is_active = (i == active_index_);

        if (is_active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        const std::string& icon = tools_[(size_t)i].icon;
        // Render a normal button for interaction/styling, then overlay scaled icon text.
        if (ImGui::Button("##tool_btn", button_size))
        {
            if (active_index_ != i)
            {
                active_index_ = i;
                active_changed_ = true;
                changed_this_frame = true;
            }
        }
        {
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            draw_centered_scaled_text(item_min, item_max, icon.empty() ? "?" : icon);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("%s", tools_[(size_t)i].label.c_str());
            ImGui::TextDisabled("%s", tools_[(size_t)i].path.c_str());
            ImGui::EndTooltip();
        }

        if (is_active)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return changed_this_frame;
}

