#include "io/session/session_state.h"

#include "core/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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
    j["schema_version"] = 18;

    json win;
    win["w"] = st.window_w;
    win["h"] = st.window_h;
    win["x"] = st.window_x;
    win["y"] = st.window_y;
    win["pos_valid"] = st.window_pos_valid;
    win["maximized"] = st.window_maximized;
    win["fullscreen"] = st.window_fullscreen;
    j["window"] = std::move(win);

    json ui;
    ui["show_color_picker_window"] = st.show_color_picker_window;
    ui["show_character_picker_window"] = st.show_character_picker_window;
    ui["show_character_palette_window"] = st.show_character_palette_window;
    ui["show_character_sets_window"] = st.show_character_sets_window;
    ui["show_layer_manager_window"] = st.show_layer_manager_window;
    ui["show_ansl_editor_window"] = st.show_ansl_editor_window;
    ui["show_tool_palette_window"] = st.show_tool_palette_window;
    ui["show_brush_palette_window"] = st.show_brush_palette_window;
    ui["show_minimap_window"] = st.show_minimap_window;
    ui["show_settings_window"] = st.show_settings_window;
    ui["show_16colors_browser_window"] = st.show_16colors_browser_window;
    if (!st.ui_theme.empty())
        ui["theme"] = st.ui_theme;
    ui["undo_limit"] = st.undo_limit;
    ui["lut_cache_budget_bytes"] = st.lut_cache_budget_bytes;
    ui["glyph_atlas_cache_budget_bytes"] = st.glyph_atlas_cache_budget_bytes;
    ui["canvas_bg_white"] = st.canvas_bg_white;
    ui["character_palette_settings_open"] = st.character_palette_settings_open;

    // Xterm-256 picker UI state
    json xcp;
    xcp["fg"] = {st.xterm_color_picker.fg[0], st.xterm_color_picker.fg[1],
                 st.xterm_color_picker.fg[2], st.xterm_color_picker.fg[3]};
    xcp["bg"] = {st.xterm_color_picker.bg[0], st.xterm_color_picker.bg[1],
                 st.xterm_color_picker.bg[2], st.xterm_color_picker.bg[3]};
    xcp["active_fb"] = st.xterm_color_picker.active_fb;
    xcp["picker_mode"] = st.xterm_color_picker.picker_mode;
    xcp["selected_palette"] = st.xterm_color_picker.selected_palette;
    xcp["picker_preview_fb"] = st.xterm_color_picker.picker_preview_fb;
    xcp["last_hue"] = st.xterm_color_picker.last_hue;
    ui["xterm_color_picker"] = std::move(xcp);

    // ANSL editor state (script text + dropdown selection + fps).
    {
        json ae;
        ae["target_fps"] = st.ansl_editor.target_fps;
        ae["selected_example_index"] = st.ansl_editor.selected_example_index;
        if (!st.ansl_editor.selected_example_label.empty())
            ae["selected_example_label"] = st.ansl_editor.selected_example_label;
        if (!st.ansl_editor.selected_example_path.empty())
            ae["selected_example_path"] = st.ansl_editor.selected_example_path;
        if (st.ansl_editor.text_valid)
            ae["text"] = st.ansl_editor.text;
        ui["ansl_editor"] = std::move(ae);
    }

    j["ui"] = std::move(ui);

    json ws;
    if (!st.last_import_image_dir.empty())
        ws["last_import_image_dir"] = st.last_import_image_dir;
    if (!st.recent_files.empty())
        ws["recent_files"] = st.recent_files;
    j["workspace"] = std::move(ws);

    // Workspace content
    json content;
    if (!st.active_tool_path.empty())
        content["active_tool_path"] = st.active_tool_path;
    content["last_active_canvas_id"] = st.last_active_canvas_id;
    content["next_canvas_id"] = st.next_canvas_id;
    content["next_image_id"] = st.next_image_id;

    // Tool params (per-tool, persisted)
    // Schema:
    // content.tool_params[tool_id][param_key] = { type=0..4, b=bool, i=int, f=float, s=string }
    if (!st.tool_param_values.empty())
    {
        json tp = json::object();
        for (const auto& tool_kv : st.tool_param_values)
        {
            const std::string& tool_id = tool_kv.first;
            const auto& params = tool_kv.second;
            if (tool_id.empty() || params.empty())
                continue;
            json pj = json::object();
            for (const auto& p : params)
            {
                const std::string& key = p.first;
                const SessionState::ToolParamValue& v = p.second;
                if (key.empty())
                    continue;
                json vj;
                vj["type"] = v.type;
                // Only store relevant fields; keep it small.
                if (v.type == 0 || v.type == 4) vj["b"] = v.b;
                else if (v.type == 1) vj["i"] = v.i;
                else if (v.type == 2) vj["f"] = v.f;
                else if (v.type == 3) vj["s"] = v.s;
                pj[key] = std::move(vj);
            }
            if (!pj.empty())
                tp[tool_id] = std::move(pj);
        }
        if (!tp.empty())
            content["tool_params"] = std::move(tp);
    }

    // Open canvases
    json canvases = json::array();
    for (const auto& c : st.open_canvases)
    {
        json jc;
        jc["id"] = c.id;
        jc["open"] = c.open;
        if (!c.file_path.empty())
            jc["file_path"] = c.file_path;
        if (!c.project_phos_cache_rel.empty())
            jc["project_phos_cache_rel"] = c.project_phos_cache_rel;

        // Legacy embedded payload (only write if cache path is absent).
        if (c.project_phos_cache_rel.empty())
        {
            jc["project_cbor_size"] = c.project_cbor_size;
            jc["project_cbor_zstd_b64"] = c.project_cbor_zstd_b64;
        }
        jc["zoom"] = c.zoom;
        jc["scroll_x"] = c.scroll_x;
        jc["scroll_y"] = c.scroll_y;
        jc["canvas_bg_white"] = c.canvas_bg_white;
        if (c.active_glyph_cp != 0)
            jc["active_glyph_cp"] = c.active_glyph_cp;
        if (!c.active_glyph_utf8.empty())
            jc["active_glyph_utf8"] = c.active_glyph_utf8;
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

    // ImGui window chrome (opacity + z-order pinning)
    json chrome = json::object();
    for (const auto& kv : st.imgui_window_chrome)
    {
        const std::string& name = kv.first;
        const SessionState::ImGuiWindowChromeState& c = kv.second;

        const float opacity = std::clamp(c.opacity, 0.05f, 1.0f);
        const int z = (c.z_order < 0) ? 0 : (c.z_order > 2) ? 2 : c.z_order;

        // Don't persist defaults.
        if (opacity == 1.0f && z == 0)
            continue;

        json jc;
        jc["opacity"] = opacity;
        jc["z_order"] = z;
        chrome[name] = std::move(jc);
    }
    j["imgui_window_chrome"] = std::move(chrome);

    // Textmode font sanity cache (broken FIGlet/TDF ids).
    {
        json fc;
        fc["schema_version"] = st.font_sanity_cache.schema_version;
        fc["fonts_fingerprint"] = st.font_sanity_cache.fonts_fingerprint;
        fc["complete"] = st.font_sanity_cache.complete;
        if (!st.font_sanity_cache.broken_ids.empty())
            fc["broken_ids"] = st.font_sanity_cache.broken_ids;
        j["font_sanity_cache"] = std::move(fc);
    }

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
        if (w.contains("fullscreen") && w["fullscreen"].is_boolean()) out.window_fullscreen = w["fullscreen"].get<bool>();
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
        if (ui.contains("show_character_sets_window") && ui["show_character_sets_window"].is_boolean())
            out.show_character_sets_window = ui["show_character_sets_window"].get<bool>();
        if (ui.contains("show_layer_manager_window") && ui["show_layer_manager_window"].is_boolean())
            out.show_layer_manager_window = ui["show_layer_manager_window"].get<bool>();
        if (ui.contains("show_ansl_editor_window") && ui["show_ansl_editor_window"].is_boolean())
            out.show_ansl_editor_window = ui["show_ansl_editor_window"].get<bool>();
        if (ui.contains("show_tool_palette_window") && ui["show_tool_palette_window"].is_boolean())
            out.show_tool_palette_window = ui["show_tool_palette_window"].get<bool>();
        if (ui.contains("show_brush_palette_window") && ui["show_brush_palette_window"].is_boolean())
            out.show_brush_palette_window = ui["show_brush_palette_window"].get<bool>();
        // Rename/migration: Preview -> Minimap
        if (ui.contains("show_minimap_window") && ui["show_minimap_window"].is_boolean())
            out.show_minimap_window = ui["show_minimap_window"].get<bool>();
        else if (ui.contains("show_preview_window") && ui["show_preview_window"].is_boolean())
            out.show_minimap_window = ui["show_preview_window"].get<bool>();
        if (ui.contains("show_settings_window") && ui["show_settings_window"].is_boolean())
            out.show_settings_window = ui["show_settings_window"].get<bool>();
        if (ui.contains("show_16colors_browser_window") && ui["show_16colors_browser_window"].is_boolean())
            out.show_16colors_browser_window = ui["show_16colors_browser_window"].get<bool>();
        if (ui.contains("theme") && ui["theme"].is_string())
            out.ui_theme = ui["theme"].get<std::string>();
        if (ui.contains("undo_limit") && ui["undo_limit"].is_number_unsigned())
            out.undo_limit = ui["undo_limit"].get<size_t>();
        else if (ui.contains("undo_limit") && ui["undo_limit"].is_number_integer())
        {
            const int v = ui["undo_limit"].get<int>();
            out.undo_limit = (v > 0) ? static_cast<size_t>(v) : 0;
        }

        if (ui.contains("lut_cache_budget_bytes") && ui["lut_cache_budget_bytes"].is_number_unsigned())
            out.lut_cache_budget_bytes = ui["lut_cache_budget_bytes"].get<size_t>();
        else if (ui.contains("lut_cache_budget_bytes") && ui["lut_cache_budget_bytes"].is_number_integer())
        {
            const int v = ui["lut_cache_budget_bytes"].get<int>();
            out.lut_cache_budget_bytes = (v > 0) ? static_cast<size_t>(v) : 0;
        }

        if (ui.contains("glyph_atlas_cache_budget_bytes") && ui["glyph_atlas_cache_budget_bytes"].is_number_unsigned())
            out.glyph_atlas_cache_budget_bytes = ui["glyph_atlas_cache_budget_bytes"].get<size_t>();
        else if (ui.contains("glyph_atlas_cache_budget_bytes") && ui["glyph_atlas_cache_budget_bytes"].is_number_integer())
        {
            const int v = ui["glyph_atlas_cache_budget_bytes"].get<int>();
            out.glyph_atlas_cache_budget_bytes = (v > 0) ? static_cast<size_t>(v) : 0;
        }
        if (ui.contains("canvas_bg_white") && ui["canvas_bg_white"].is_boolean())
            out.canvas_bg_white = ui["canvas_bg_white"].get<bool>();
        if (ui.contains("character_palette_settings_open") && ui["character_palette_settings_open"].is_boolean())
            out.character_palette_settings_open = ui["character_palette_settings_open"].get<bool>();

        if (ui.contains("xterm_color_picker") && ui["xterm_color_picker"].is_object())
        {
            const json& xcp = ui["xterm_color_picker"];
            if (xcp.contains("fg") && xcp["fg"].is_array() && xcp["fg"].size() == 4)
            {
                for (int i = 0; i < 4; ++i)
                    if (xcp["fg"][i].is_number())
                        out.xterm_color_picker.fg[i] = xcp["fg"][i].get<float>();
            }
            if (xcp.contains("bg") && xcp["bg"].is_array() && xcp["bg"].size() == 4)
            {
                for (int i = 0; i < 4; ++i)
                    if (xcp["bg"][i].is_number())
                        out.xterm_color_picker.bg[i] = xcp["bg"][i].get<float>();
            }
            if (xcp.contains("active_fb") && xcp["active_fb"].is_number_integer())
                out.xterm_color_picker.active_fb = xcp["active_fb"].get<int>();
            if (xcp.contains("picker_mode") && xcp["picker_mode"].is_number_integer())
                out.xterm_color_picker.picker_mode = xcp["picker_mode"].get<int>();
            if (xcp.contains("selected_palette") && xcp["selected_palette"].is_number_integer())
                out.xterm_color_picker.selected_palette = xcp["selected_palette"].get<int>();
            if (xcp.contains("picker_preview_fb") && xcp["picker_preview_fb"].is_number_integer())
                out.xterm_color_picker.picker_preview_fb = xcp["picker_preview_fb"].get<int>();
            if (xcp.contains("last_hue") && xcp["last_hue"].is_number())
                out.xterm_color_picker.last_hue = xcp["last_hue"].get<float>();
        }

        if (ui.contains("ansl_editor") && ui["ansl_editor"].is_object())
        {
            const json& ae = ui["ansl_editor"];
            if (ae.contains("target_fps") && ae["target_fps"].is_number_integer())
            {
                const int fps = ae["target_fps"].get<int>();
                out.ansl_editor.target_fps = std::clamp(fps, 1, 240);
            }
            if (ae.contains("selected_example_index") && ae["selected_example_index"].is_number_integer())
                out.ansl_editor.selected_example_index = ae["selected_example_index"].get<int>();
            if (ae.contains("selected_example_label") && ae["selected_example_label"].is_string())
                out.ansl_editor.selected_example_label = ae["selected_example_label"].get<std::string>();
            if (ae.contains("selected_example_path") && ae["selected_example_path"].is_string())
                out.ansl_editor.selected_example_path = ae["selected_example_path"].get<std::string>();
            if (ae.contains("text") && ae["text"].is_string())
            {
                out.ansl_editor.text_valid = true;
                out.ansl_editor.text = ae["text"].get<std::string>();
            }

            // Basic sanity clamp so broken state doesn't break the UI.
            if (out.ansl_editor.selected_example_index < -1)
                out.ansl_editor.selected_example_index = -1;
        }
    }

    if (j.contains("workspace") && j["workspace"].is_object())
    {
        const json& ws = j["workspace"];
        if (ws.contains("last_import_image_dir") && ws["last_import_image_dir"].is_string())
            out.last_import_image_dir = ws["last_import_image_dir"].get<std::string>();
        if (ws.contains("recent_files") && ws["recent_files"].is_array())
        {
            out.recent_files.clear();
            for (const auto& v : ws["recent_files"])
            {
                if (v.is_string())
                    out.recent_files.push_back(v.get<std::string>());
            }
        }
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

        // Tool params (optional)
        out.tool_param_values.clear();
        if (c.contains("tool_params") && c["tool_params"].is_object())
        {
            const json& tp = c["tool_params"];
            for (auto it_tool = tp.begin(); it_tool != tp.end(); ++it_tool)
            {
                const std::string tool_id = it_tool.key();
                if (tool_id.empty() || !it_tool.value().is_object())
                    continue;
                std::unordered_map<std::string, SessionState::ToolParamValue> params;
                const json& pj = it_tool.value();
                for (auto it_p = pj.begin(); it_p != pj.end(); ++it_p)
                {
                    const std::string key = it_p.key();
                    if (key.empty() || !it_p.value().is_object())
                        continue;
                    const json& vj = it_p.value();
                    SessionState::ToolParamValue v;
                    if (vj.contains("type") && vj["type"].is_number_integer())
                        v.type = vj["type"].get<int>();
                    // Clamp to known range so corrupt state doesn't break parsing.
                    if (v.type < 0) v.type = 0;
                    if (v.type > 4) v.type = 4;

                    if (vj.contains("b") && vj["b"].is_boolean())
                        v.b = vj["b"].get<bool>();
                    if (vj.contains("i") && vj["i"].is_number_integer())
                        v.i = vj["i"].get<int>();
                    if (vj.contains("f") && vj["f"].is_number())
                        v.f = vj["f"].get<float>();
                    if (vj.contains("s") && vj["s"].is_string())
                        v.s = vj["s"].get<std::string>();

                    params[key] = std::move(v);
                }
                if (!params.empty())
                    out.tool_param_values[tool_id] = std::move(params);
            }
        }

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
                if (jc.contains("file_path") && jc["file_path"].is_string())
                    oc.file_path = jc["file_path"].get<std::string>();
                if (jc.contains("project_phos_cache_rel") && jc["project_phos_cache_rel"].is_string())
                    oc.project_phos_cache_rel = jc["project_phos_cache_rel"].get<std::string>();
                if (jc.contains("project_cbor_size") && (jc["project_cbor_size"].is_number_unsigned() || jc["project_cbor_size"].is_number_integer()))
                    oc.project_cbor_size = jc["project_cbor_size"].get<std::uint64_t>();
                if (jc.contains("project_cbor_zstd_b64") && jc["project_cbor_zstd_b64"].is_string())
                    oc.project_cbor_zstd_b64 = jc["project_cbor_zstd_b64"].get<std::string>();
                if (jc.contains("zoom") && jc["zoom"].is_number()) oc.zoom = jc["zoom"].get<float>();
                if (jc.contains("scroll_x") && jc["scroll_x"].is_number()) oc.scroll_x = jc["scroll_x"].get<float>();
                if (jc.contains("scroll_y") && jc["scroll_y"].is_number()) oc.scroll_y = jc["scroll_y"].get<float>();
                if (jc.contains("canvas_bg_white") && jc["canvas_bg_white"].is_boolean())
                    oc.canvas_bg_white = jc["canvas_bg_white"].get<bool>();
                if (jc.contains("active_glyph_cp") && (jc["active_glyph_cp"].is_number_unsigned() || jc["active_glyph_cp"].is_number_integer()))
                    oc.active_glyph_cp = jc["active_glyph_cp"].get<std::uint32_t>();
                if (jc.contains("active_glyph_utf8") && jc["active_glyph_utf8"].is_string())
                    oc.active_glyph_utf8 = jc["active_glyph_utf8"].get<std::string>();

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

    // ImGui window chrome (opacity + z-order pinning)
    if (j.contains("imgui_window_chrome") && j["imgui_window_chrome"].is_object())
    {
        const json& chrome = j["imgui_window_chrome"];
        for (auto it = chrome.begin(); it != chrome.end(); ++it)
        {
            if (!it.value().is_object())
                continue;

            SessionState::ImGuiWindowChromeState c;
            const json& jc = it.value();
            if (jc.contains("opacity") && jc["opacity"].is_number())
                c.opacity = std::clamp(jc["opacity"].get<float>(), 0.05f, 1.0f);
            if (jc.contains("z_order") && jc["z_order"].is_number_integer())
                c.z_order = std::clamp(jc["z_order"].get<int>(), 0, 2);

            // Only store non-defaults to keep the map small.
            if (c.opacity != 1.0f || c.z_order != 0)
                out.imgui_window_chrome[it.key()] = c;
        }
    }

    // Rename/migration: "Preview" window -> "Minimap"
    // - Move placement/chrome state so we don't keep stale keys forever.
    {
        auto migrate_key = [](auto& map, const char* old_key, const char* new_key)
        {
            const auto it_old = map.find(old_key);
            const auto it_new = map.find(new_key);
            if (it_old != map.end() && it_new == map.end())
            {
                map[new_key] = it_old->second;
                map.erase(it_old);
            }
        };
        migrate_key(out.imgui_windows, "Preview", "Minimap");
        migrate_key(out.imgui_window_chrome, "Preview", "Minimap");
    }

    // Migration: collapse per-instance canvas/image placement keys into stable per-document keys.
    // This prevents unbounded growth when the same file is opened repeatedly (canvas ids / image ids keep increasing).
    {
        auto sanitize_id = [](std::string s) -> std::string {
            for (;;)
            {
                const size_t pos = s.find("##");
                if (pos == std::string::npos)
                    break;
                s.replace(pos, 2, "#");
            }
            return s;
        };

        auto starts_with = [](const std::string& s, const std::string& prefix) -> bool {
            return s.size() >= prefix.size() && s.rfind(prefix, 0) == 0;
        };

        auto parse_trailing_int_after_hash = [](const std::string& s) -> int {
            const size_t pos = s.rfind('#');
            if (pos == std::string::npos || pos + 1 >= s.size())
                return -1;
            try
            {
                const long long v = std::stoll(s.substr(pos + 1));
                if (v < 0 || v > (long long)std::numeric_limits<int>::max())
                    return -1;
                return (int)v;
            }
            catch (...)
            {
                return -1;
            }
        };

        auto extract_canvas_path = [&](const std::string& key, std::string& out_path) -> bool {
            out_path.clear();
            const std::string needle = "canvas:";
            const size_t pos = key.find(needle);
            if (pos == std::string::npos)
                return false;
            const size_t start = pos + needle.size();
            if (start >= key.size())
                return false;
            size_t end = key.find('#', start);
            if (end == std::string::npos)
                end = key.size();
            if (end <= start)
                return false;
            out_path = key.substr(start, end - start);
            return !out_path.empty();
        };

        auto extract_image_path_from_legacy_key = [&](const std::string& key, std::string& out_path) -> bool {
            // Legacy image window ids used the *title* as the persistence key:
            //   "<path>##image:<path>#<id>"
            out_path.clear();
            const std::string needle = "##image:";
            const size_t pos = key.find(needle);
            if (pos == std::string::npos)
                return false;
            const size_t start = pos + needle.size(); // points to "<path>#<id>" (path can contain ':', '/', etc)
            if (start >= key.size())
                return false;
            const size_t hash = key.rfind('#');
            if (hash == std::string::npos || hash <= start)
                return false;
            out_path = key.substr(start, hash - start);
            return !out_path.empty();
        };

        const std::string session_canvas_dir = PhosphorCachePath("session_canvases");

        // Collapse canvas placement keys:
        // - keep temp session canvases per-instance (they get pruned by file existence elsewhere)
        // - collapse all other canvas keys to "canvas:<path>"
        std::unordered_map<std::string, std::pair<ImGuiWindowPlacement, int>> best_canvas;
        std::vector<std::string> erase_canvas_keys;
        best_canvas.reserve(out.imgui_windows.size());
        erase_canvas_keys.reserve(out.imgui_windows.size());

        for (const auto& kv : out.imgui_windows)
        {
            const std::string& key = kv.first;
            const ImGuiWindowPlacement& p = kv.second;
            std::string path;
            if (!extract_canvas_path(key, path))
                continue;
            path = sanitize_id(path);

            // Do not collapse temp session canvases (under <config>/cache/session_canvases).
            if (!session_canvas_dir.empty() && starts_with(path, session_canvas_dir))
                continue;

            const std::string canonical = "canvas:" + path;
            const int score = parse_trailing_int_after_hash(key);

            auto it = best_canvas.find(canonical);
            if (it == best_canvas.end() || score > it->second.second)
                best_canvas[canonical] = std::make_pair(p, score);

            if (key != canonical)
                erase_canvas_keys.push_back(key);
        }
        for (const auto& k : erase_canvas_keys)
            out.imgui_windows.erase(k);
        for (auto& kv : best_canvas)
        {
            if (out.imgui_windows.find(kv.first) == out.imgui_windows.end())
                out.imgui_windows[kv.first] = kv.second.first;
        }

        // Collapse legacy image placement keys to stable keys:
        // - new key will be "image:<path>"
        std::unordered_map<std::string, std::pair<ImGuiWindowPlacement, int>> best_image;
        std::vector<std::string> erase_image_keys;
        best_image.reserve(out.imgui_windows.size());
        erase_image_keys.reserve(out.imgui_windows.size());

        for (const auto& kv : out.imgui_windows)
        {
            const std::string& key = kv.first;
            const ImGuiWindowPlacement& p = kv.second;

            // Already stable form.
            if (starts_with(key, "image:"))
                continue;

            std::string path;
            if (!extract_image_path_from_legacy_key(key, path))
                continue;
            path = sanitize_id(path);

            const std::string canonical = "image:" + path;
            const int score = parse_trailing_int_after_hash(key);

            auto it = best_image.find(canonical);
            if (it == best_image.end() || score > it->second.second)
                best_image[canonical] = std::make_pair(p, score);

            erase_image_keys.push_back(key);
        }
        for (const auto& k : erase_image_keys)
            out.imgui_windows.erase(k);
        for (auto& kv : best_image)
        {
            if (out.imgui_windows.find(kv.first) == out.imgui_windows.end())
                out.imgui_windows[kv.first] = kv.second.first;
        }
    }

    // Font sanity cache (optional).
    if (j.contains("font_sanity_cache") && j["font_sanity_cache"].is_object())
    {
        const json& fc = j["font_sanity_cache"];
        if (fc.contains("schema_version") && fc["schema_version"].is_number_integer())
            out.font_sanity_cache.schema_version = fc["schema_version"].get<int>();
        if (fc.contains("fonts_fingerprint") &&
            (fc["fonts_fingerprint"].is_number_unsigned() || fc["fonts_fingerprint"].is_number_integer()))
            out.font_sanity_cache.fonts_fingerprint = fc["fonts_fingerprint"].get<std::uint64_t>();
        if (fc.contains("complete") && fc["complete"].is_boolean())
            out.font_sanity_cache.complete = fc["complete"].get<bool>();
        if (fc.contains("broken_ids") && fc["broken_ids"].is_array())
        {
            out.font_sanity_cache.broken_ids.clear();
            for (const auto& v : fc["broken_ids"])
            {
                if (v.is_string())
                    out.font_sanity_cache.broken_ids.push_back(v.get<std::string>());
            }
        }
    }

    // Brush palette (global)
    if (j.contains("brush_palette") && j["brush_palette"].is_object())
    {
        const json& bp = j["brush_palette"];
        if (bp.contains("version") && bp["version"].is_number_integer())
            out.brush_palette.version = bp["version"].get<int>();
        if (bp.contains("selected") && bp["selected"].is_number_integer())
            out.brush_palette.selected = bp["selected"].get<int>();

        out.brush_palette.entries.clear();
        if (bp.contains("entries") && bp["entries"].is_array())
        {
            for (const auto& je : bp["entries"])
            {
                if (!je.is_object())
                    continue;
                SessionState::BrushPaletteEntry e;
                if (je.contains("name") && je["name"].is_string())
                    e.name = je["name"].get<std::string>();
                if (je.contains("w") && je["w"].is_number_integer())
                    e.w = je["w"].get<int>();
                if (je.contains("h") && je["h"].is_number_integer())
                    e.h = je["h"].get<int>();

                auto load_u32_vec = [&](const char* key, std::vector<std::uint32_t>& dst) {
                    dst.clear();
                    if (!je.contains(key) || !je[key].is_array())
                        return;
                    for (const auto& v : je[key])
                    {
                        if (v.is_number_unsigned() || v.is_number_integer())
                            dst.push_back(v.get<std::uint32_t>());
                    }
                };
                load_u32_vec("cp", e.cp);
                load_u32_vec("fg", e.fg);
                load_u32_vec("bg", e.bg);
                load_u32_vec("attrs", e.attrs);

                // Basic validation: dimensions must match payload if present.
                if (e.w <= 0 || e.h <= 0)
                    continue;
                const size_t n = (size_t)e.w * (size_t)e.h;
                if (!e.cp.empty() && e.cp.size() != n) continue;
                if (!e.fg.empty() && e.fg.size() != n) continue;
                if (!e.bg.empty() && e.bg.size() != n) continue;
                if (!e.attrs.empty() && e.attrs.size() != n) continue;

                out.brush_palette.entries.push_back(std::move(e));
            }
        }
        // Clamp selection
        if (out.brush_palette.selected < -1) out.brush_palette.selected = -1;
        if (out.brush_palette.selected >= (int)out.brush_palette.entries.size())
            out.brush_palette.selected = (int)out.brush_palette.entries.size() - 1;
    }
}

bool LoadSessionState(SessionState& out, std::string& err)
{
    err.clear();
    const std::string path = GetSessionStatePath();

    std::ifstream f(path);
    if (!f)
    {
        // If the user session file doesn't exist yet (first run), load the bundled defaults
        // from "<config_dir>/assets/session.json" if available.
        std::error_code ec;
        const bool user_session_exists = fs::exists(path, ec);
        if (user_session_exists && !ec)
        {
            err = std::string("Failed to open session state file for reading: ") + path;
            return false;
        }

        const std::string default_path = PhosphorAssetPath("session.json");
        std::ifstream df(default_path);
        if (!df)
            return true; // no user session and no default; use hardcoded defaults

        json dj;
        try
        {
            df >> dj;
        }
        catch (const std::exception& e)
        {
            err = std::string("Failed to parse default session state (") + default_path + "): " + e.what();
            return false;
        }

        // Basic schema check (but keep it forgiving).
        if (dj.contains("schema_version") && dj["schema_version"].is_number_integer())
        {
            const int ver = dj["schema_version"].get<int>();
            if (ver != 1 && ver != 2 && ver != 3 && ver != 4 && ver != 5 && ver != 6 && ver != 7 && ver != 8 &&
                ver != 9 && ver != 10 && ver != 11 && ver != 12 && ver != 13 && ver != 14 && ver != 15 && ver != 16 &&
                ver != 17)
            {
                // Unknown schema: ignore file rather than failing startup.
                return true;
            }
        }

        FromJson(dj, out);
        return true;
    }

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
        if (ver != 1 && ver != 2 && ver != 3 && ver != 4 && ver != 5 && ver != 6 && ver != 7 && ver != 8 &&
            ver != 9 && ver != 10 && ver != 11 && ver != 12 && ver != 13 && ver != 14 && ver != 15 && ver != 16 &&
            ver != 17 && ver != 18)
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

    // Atomic write: write to a temp file in the same directory then rename over the original.
    const std::string tmp_path = path + ".tmp";

    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        err = "Failed to open temp session state file for writing.";
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

    out.close();
    if (!out)
    {
        err = "Failed to finalize session state temp file write.";
        return false;
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec)
    {
        err = std::string("Failed to atomically replace session state file: ") + ec.message();
        // Best effort cleanup; ignore errors.
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        return false;
    }

    return true;
}


