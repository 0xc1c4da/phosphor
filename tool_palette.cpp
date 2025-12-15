#include "tool_palette.h"

#include "imgui.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

bool ToolPalette::ParseToolSettingsFromLuaFile(const std::string& path, ToolSpec& out, std::string& error)
{
    out = ToolSpec{};
    out.path = path;
    out.icon = "?";
    out.label = BasenameNoExt(path);

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
        lua_getfield(L, -1, "icon");
        if (lua_isstring(L, -1))
            out.icon = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "label");
        if (lua_isstring(L, -1))
            out.label = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // settings

    lua_close(L);
    error.clear();
    return true;
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

    // Stable ordering by label then path (so UI doesn't jump around).
    std::sort(found.begin(), found.end(), [](const ToolSpec& a, const ToolSpec& b) {
        if (a.label != b.label) return a.label < b.label;
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

bool ToolPalette::Render(const char* title, bool* p_open)
{
    bool changed_this_frame = false;
    if (!ImGui::Begin(title, p_open, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return false;
    }

    if (ImGui::Button("Refresh"))
    {
        reload_requested_ = true;
        changed_this_frame = true;
    }
    ImGui::Separator();

    if (tools_.empty())
    {
        ImGui::TextUnformatted("No tools loaded.");
        ImGui::End();
        return changed_this_frame;
    }

    // Icon-only buttons in a grid.
    const float btn_sz = ImGui::GetFrameHeight() * 2.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    int cols = 1;
    if (avail > btn_sz)
        cols = std::max(1, (int)std::floor(avail / (btn_sz + ImGui::GetStyle().ItemSpacing.x)));

    for (int i = 0; i < (int)tools_.size(); ++i)
    {
        if (i % cols != 0)
            ImGui::SameLine();
        ImGui::PushID(i);
        const bool is_active = (i == active_index_);

        if (is_active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        const std::string& icon = tools_[(size_t)i].icon;
        if (ImGui::Button(icon.empty() ? "?" : icon.c_str(), ImVec2(btn_sz, btn_sz)))
        {
            if (active_index_ != i)
            {
                active_index_ = i;
                active_changed_ = true;
                changed_this_frame = true;
            }
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
    return changed_this_frame;
}

