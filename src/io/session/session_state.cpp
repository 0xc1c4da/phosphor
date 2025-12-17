#include "io/session/session_state.h"

#include <nlohmann/json.hpp>
#include <zstd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static std::string EnvOrEmpty(const char* name)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

std::string GetPhosphorConfigDir()
{
    // Linux-first (your current environment), but also reasonable defaults elsewhere.
    const std::string xdg = EnvOrEmpty("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return xdg + "/phosphor";

    const std::string home = EnvOrEmpty("HOME");
    if (!home.empty())
        return home + "/.config/phosphor";

    // Last resort: current directory
    return ".";
}

std::string GetSessionStatePath()
{
    return (fs::path(GetPhosphorConfigDir()) / "session.json").string();
}

static void EnsureParentDirExists(const std::string& path, std::string& err)
{
    err.clear();
    try
    {
        fs::path p(path);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
    }
    catch (const std::exception& e)
    {
        err = e.what();
    }
}

static json ToJson(const SessionState& st)
{
    json j;
    j["schema_version"] = 2;

    json win;
    win["w"] = st.window_w;
    win["h"] = st.window_h;
    win["x"] = st.window_x;
    win["y"] = st.window_y;
    win["pos_valid"] = st.window_pos_valid;
    win["maximized"] = st.window_maximized;
    j["window"] = std::move(win);

    json ui;
    ui["show_color_picker_window"] = st.show_color_picker_window;
    ui["show_character_picker_window"] = st.show_character_picker_window;
    ui["show_character_palette_window"] = st.show_character_palette_window;
    ui["show_layer_manager_window"] = st.show_layer_manager_window;
    ui["show_ansl_editor_window"] = st.show_ansl_editor_window;
    ui["show_tool_palette_window"] = st.show_tool_palette_window;
    ui["show_preview_window"] = st.show_preview_window;
    ui["show_settings_window"] = st.show_settings_window;
    j["ui"] = std::move(ui);

    json ws;
    if (!st.last_import_image_dir.empty())
        ws["last_import_image_dir"] = st.last_import_image_dir;
    j["workspace"] = std::move(ws);

    // Workspace content
    json content;
    if (!st.active_tool_path.empty())
        content["active_tool_path"] = st.active_tool_path;
    content["last_active_canvas_id"] = st.last_active_canvas_id;
    content["next_canvas_id"] = st.next_canvas_id;
    content["next_image_id"] = st.next_image_id;

    // Open canvases
    json canvases = json::array();
    for (const auto& c : st.open_canvases)
    {
        json jc;
        jc["id"] = c.id;
        jc["open"] = c.open;
        jc["project_cbor_size"] = c.project_cbor_size;
        jc["project_cbor_zstd_b64"] = c.project_cbor_zstd_b64;
        jc["zoom"] = c.zoom;
        jc["scroll_x"] = c.scroll_x;
        jc["scroll_y"] = c.scroll_y;
        canvases.push_back(std::move(jc));
    }
    content["open_canvases"] = std::move(canvases);

    // Open images
    json images = json::array();
    for (const auto& im : st.open_images)
    {
        json ji;
        ji["id"] = im.id;
        ji["open"] = im.open;
        ji["path"] = im.path;
        images.push_back(std::move(ji));
    }
    content["open_images"] = std::move(images);

    j["content"] = std::move(content);

    // ImGui window placements
    json wins = json::object();
    for (const auto& kv : st.imgui_windows)
    {
        const std::string& name = kv.first;
        const ImGuiWindowPlacement& p = kv.second;
        if (!p.valid)
            continue;
        json w;
        w["x"] = p.x;
        w["y"] = p.y;
        w["w"] = p.w;
        w["h"] = p.h;
        w["collapsed"] = p.collapsed;
        wins[name] = std::move(w);
    }
    j["imgui_windows"] = std::move(wins);

    return j;
}

static void FromJson(const json& j, SessionState& out)
{
    // Defaults are already in out; only override what we recognize.
    if (j.contains("window") && j["window"].is_object())
    {
        const json& w = j["window"];
        if (w.contains("w") && w["w"].is_number_integer()) out.window_w = w["w"].get<int>();
        if (w.contains("h") && w["h"].is_number_integer()) out.window_h = w["h"].get<int>();
        if (w.contains("x") && w["x"].is_number_integer()) out.window_x = w["x"].get<int>();
        if (w.contains("y") && w["y"].is_number_integer()) out.window_y = w["y"].get<int>();
        if (w.contains("pos_valid") && w["pos_valid"].is_boolean()) out.window_pos_valid = w["pos_valid"].get<bool>();
        if (w.contains("maximized") && w["maximized"].is_boolean()) out.window_maximized = w["maximized"].get<bool>();
    }

    if (j.contains("ui") && j["ui"].is_object())
    {
        const json& ui = j["ui"];
        if (ui.contains("show_color_picker_window") && ui["show_color_picker_window"].is_boolean())
            out.show_color_picker_window = ui["show_color_picker_window"].get<bool>();
        if (ui.contains("show_character_picker_window") && ui["show_character_picker_window"].is_boolean())
            out.show_character_picker_window = ui["show_character_picker_window"].get<bool>();
        if (ui.contains("show_character_palette_window") && ui["show_character_palette_window"].is_boolean())
            out.show_character_palette_window = ui["show_character_palette_window"].get<bool>();
        if (ui.contains("show_layer_manager_window") && ui["show_layer_manager_window"].is_boolean())
            out.show_layer_manager_window = ui["show_layer_manager_window"].get<bool>();
        if (ui.contains("show_ansl_editor_window") && ui["show_ansl_editor_window"].is_boolean())
            out.show_ansl_editor_window = ui["show_ansl_editor_window"].get<bool>();
        if (ui.contains("show_tool_palette_window") && ui["show_tool_palette_window"].is_boolean())
            out.show_tool_palette_window = ui["show_tool_palette_window"].get<bool>();
        if (ui.contains("show_preview_window") && ui["show_preview_window"].is_boolean())
            out.show_preview_window = ui["show_preview_window"].get<bool>();
        if (ui.contains("show_settings_window") && ui["show_settings_window"].is_boolean())
            out.show_settings_window = ui["show_settings_window"].get<bool>();
    }

    if (j.contains("workspace") && j["workspace"].is_object())
    {
        const json& ws = j["workspace"];
        if (ws.contains("last_import_image_dir") && ws["last_import_image_dir"].is_string())
            out.last_import_image_dir = ws["last_import_image_dir"].get<std::string>();
    }

    if (j.contains("content") && j["content"].is_object())
    {
        const json& c = j["content"];
        if (c.contains("active_tool_path") && c["active_tool_path"].is_string())
            out.active_tool_path = c["active_tool_path"].get<std::string>();
        if (c.contains("last_active_canvas_id") && c["last_active_canvas_id"].is_number_integer())
            out.last_active_canvas_id = c["last_active_canvas_id"].get<int>();
        if (c.contains("next_canvas_id") && c["next_canvas_id"].is_number_integer())
            out.next_canvas_id = c["next_canvas_id"].get<int>();
        if (c.contains("next_image_id") && c["next_image_id"].is_number_integer())
            out.next_image_id = c["next_image_id"].get<int>();

        out.open_canvases.clear();
        if (c.contains("open_canvases") && c["open_canvases"].is_array())
        {
            for (const auto& jc : c["open_canvases"])
            {
                if (!jc.is_object())
                    continue;
                SessionState::OpenCanvas oc;
                if (jc.contains("id") && jc["id"].is_number_integer()) oc.id = jc["id"].get<int>();
                if (jc.contains("open") && jc["open"].is_boolean()) oc.open = jc["open"].get<bool>();
                if (jc.contains("project_cbor_size") && (jc["project_cbor_size"].is_number_unsigned() || jc["project_cbor_size"].is_number_integer()))
                    oc.project_cbor_size = jc["project_cbor_size"].get<std::uint64_t>();
                if (jc.contains("project_cbor_zstd_b64") && jc["project_cbor_zstd_b64"].is_string())
                    oc.project_cbor_zstd_b64 = jc["project_cbor_zstd_b64"].get<std::string>();
                if (jc.contains("zoom") && jc["zoom"].is_number()) oc.zoom = jc["zoom"].get<float>();
                if (jc.contains("scroll_x") && jc["scroll_x"].is_number()) oc.scroll_x = jc["scroll_x"].get<float>();
                if (jc.contains("scroll_y") && jc["scroll_y"].is_number()) oc.scroll_y = jc["scroll_y"].get<float>();

                if (oc.id > 0)
                    out.open_canvases.push_back(std::move(oc));
            }
        }

        out.open_images.clear();
        if (c.contains("open_images") && c["open_images"].is_array())
        {
            for (const auto& ji : c["open_images"])
            {
                if (!ji.is_object())
                    continue;
                SessionState::OpenImage im;
                if (ji.contains("id") && ji["id"].is_number_integer()) im.id = ji["id"].get<int>();
                if (ji.contains("open") && ji["open"].is_boolean()) im.open = ji["open"].get<bool>();
                if (ji.contains("path") && ji["path"].is_string()) im.path = ji["path"].get<std::string>();
                if (im.id > 0 && !im.path.empty())
                    out.open_images.push_back(std::move(im));
            }
        }
    }

    // ImGui window placements
    if (j.contains("imgui_windows") && j["imgui_windows"].is_object())
    {
        const json& wins = j["imgui_windows"];
        for (auto it = wins.begin(); it != wins.end(); ++it)
        {
            if (!it.value().is_object())
                continue;
            ImGuiWindowPlacement p;
            const json& w = it.value();
            if (w.contains("x") && w["x"].is_number()) p.x = w["x"].get<float>();
            if (w.contains("y") && w["y"].is_number()) p.y = w["y"].get<float>();
            if (w.contains("w") && w["w"].is_number()) p.w = w["w"].get<float>();
            if (w.contains("h") && w["h"].is_number()) p.h = w["h"].get<float>();
            if (w.contains("collapsed") && w["collapsed"].is_boolean()) p.collapsed = w["collapsed"].get<bool>();

            // Minimal validation so broken state doesn't cause weird windows.
            if (p.w > 1.0f && p.h > 1.0f)
                p.valid = true;

            if (p.valid)
                out.imgui_windows[it.key()] = p;
        }
    }
}

bool LoadSessionState(SessionState& out, std::string& err)
{
    err.clear();
    const std::string path = GetSessionStatePath();

    std::ifstream f(path);
    if (!f)
        return true; // missing is OK; use defaults

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        err = std::string("Failed to parse session state: ") + e.what();
        return false;
    }

    // Basic schema check (but keep it forgiving).
    if (j.contains("schema_version") && j["schema_version"].is_number_integer())
    {
        const int ver = j["schema_version"].get<int>();
        if (ver != 1 && ver != 2)
        {
            // Unknown schema: ignore file rather than failing startup.
            return true;
        }
    }

    FromJson(j, out);
    return true;
}

bool SaveSessionState(const SessionState& st, std::string& err)
{
    err.clear();
    const std::string path = GetSessionStatePath();

    std::string derr;
    EnsureParentDirExists(path, derr);
    if (!derr.empty())
    {
        err = std::string("Failed to create config directory: ") + derr;
        return false;
    }

    std::ofstream out(path);
    if (!out)
    {
        err = "Failed to open session state file for writing.";
        return false;
    }

    try
    {
        out << ToJson(st).dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        err = std::string("Failed to write session state: ") + e.what();
        return false;
    }

    return true;
}


