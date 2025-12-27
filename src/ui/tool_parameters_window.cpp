#include "ui/tool_parameters_window.h"

#include "imgui.h"

#include "core/i18n.h"
#include "io/session/imgui_persistence.h"
#include "ui/ansl_params_ui.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cstdio>

ToolParametersWindow::ToolParametersWindow() = default;

// Helpers
static const AnslParamSpec* FindParamSpec(const AnslScriptEngine& eng, const char* key);
static bool EngineHasQuickParamKey(const AnslScriptEngine& eng, const char* key);

static const AnslParamSpec* FindParamSpec(const AnslScriptEngine& eng, const char* key)
{
    if (!key || !*key)
        return nullptr;
    if (!eng.HasParams())
        return nullptr;
    for (const auto& s : eng.GetParamSpecs())
        if (s.key == key)
            return &s;
    return nullptr;
}

static bool EngineHasQuickParamKey(const AnslScriptEngine& eng, const char* key)
{
    const AnslParamSpec* s = FindParamSpec(eng, key);
    return s && (s->placement == AnslParamPlacement::Quick);
}

bool ToolParametersWindow::Render(const ToolSpec* active_tool,
                                  const std::string& compiled_tool_id,
                                  AnslScriptEngine& tool_engine,
                                  SessionState& session,
                                  bool apply_placement_this_frame)
{
    const bool has_params = tool_engine.HasParams();

    const char* base_id = "Tool Parameters";
    // Show tool label in the visible title, but keep a stable window ID for persistence.
    const std::string fallback_title = PHOS_TR("tool_parameters.window_title");
    std::string wname = std::string(active_tool ? active_tool->label : fallback_title.c_str()) + "###" + base_id;

    // Provide a sensible default size/position for first-time users, but prefer persisted placement.
    // IMPORTANT: Don't use ImGuiCond_FirstUseEver here: we use our own persistence (SessionState),
    // so "first use ever" would re-apply after every restart and effectively prevent size restore.
    if (apply_placement_this_frame)
    {
        auto it = session.imgui_windows.find(base_id);
        const bool has = (it != session.imgui_windows.end() && it->second.valid);
        if (!has)
        {
            // Match ApplyImGuiWindowPlacement's default spawn logic, but with a Tool Params-appropriate size.
            ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 work_pos = vp ? vp->WorkPos : ImVec2(0, 0);
            const ImVec2 work_size = vp ? vp->WorkSize : ImVec2(1280, 720);
            const ImVec2 center(work_pos.x + work_size.x * 0.5f,
                                work_pos.y + work_size.y * 0.5f);
            const ImVec2 default_size(420.0f, 260.0f);
            const ImVec2 top_left(center.x - default_size.x * 0.5f,
                                  center.y - default_size.y * 0.5f);
            ImGui::SetNextWindowPos(top_left, ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(default_size, ImGuiCond_Appearing);
        }
    }
    ApplyImGuiWindowPlacement(session, base_id, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session, base_id);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session, base_id);

    ImGui::Begin(wname.c_str(), nullptr, flags);
    CaptureImGuiWindowPlacement(session, base_id);
    ApplyImGuiWindowChromeZOrder(&session, base_id);
    RenderImGuiWindowChromeMenu(&session, base_id);

    bool params_changed = false;

    if (!has_params)
    {
        ImGui::TextDisabled("%s", PHOS_TR("common.no_parameters").c_str());
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return true;
    }

    // Row 2 (reserved when present): Colour row (FG/BG + related options like Source).
    // Only surface these here when the tool author marked them placement="quick".
    std::vector<const char*> skip_keys;
    skip_keys.reserve(8);
    auto add_skip = [&](const char* k) {
        if (k && *k)
            skip_keys.push_back(k);
    };

    {
        // Prefer the canonical "useFg/useBg" keys; fall back to pipette "pickFg/pickBg".
        const bool has_use_bg = EngineHasQuickParamKey(tool_engine, "useBg");
        const bool has_use_fg = EngineHasQuickParamKey(tool_engine, "useFg");
        const bool has_bg_src = EngineHasQuickParamKey(tool_engine, "bgSource");
        const bool has_fg_src = EngineHasQuickParamKey(tool_engine, "fgSource");

        const bool has_pick_bg = EngineHasQuickParamKey(tool_engine, "pickBg");
        const bool has_pick_fg = EngineHasQuickParamKey(tool_engine, "pickFg");

        const bool want_use_row = has_use_bg || has_use_fg || has_bg_src || has_fg_src;
        const bool want_pick_row = !want_use_row && (has_pick_bg || has_pick_fg);

        if (want_use_row || want_pick_row)
        {
            const ImGuiTableFlags tflags =
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("##tool_params_colour_row", 3, tflags))
            {
                ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthFixed, 0.0f);
                ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, 0.0f);
                ImGui::TableSetupColumn("##spacer", ImGuiTableColumnFlags_WidthStretch, 1.0f);

                ImGui::TableNextRow();

                // Left group (BG-ish)
                ImGui::TableSetColumnIndex(0);
                if (want_use_row)
                {
                    if (has_use_bg)
                    {
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "useBg", /*compact=*/true) || params_changed;
                        add_skip("useBg");
                    }
                    if (has_bg_src)
                    {
                        if (has_use_bg)
                            ImGui::SameLine();
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "bgSource", /*compact=*/true) || params_changed;
                        add_skip("bgSource");
                    }
                }
                else
                {
                    if (has_pick_bg)
                    {
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "pickBg", /*compact=*/true) || params_changed;
                        add_skip("pickBg");
                    }
                }

                // Right group (FG-ish)
                ImGui::TableSetColumnIndex(1);
                if (want_use_row)
                {
                    if (has_use_fg)
                    {
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "useFg", /*compact=*/true) || params_changed;
                        add_skip("useFg");
                    }
                    if (has_fg_src)
                    {
                        if (has_use_fg)
                            ImGui::SameLine();
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "fgSource", /*compact=*/true) || params_changed;
                        add_skip("fgSource");
                    }
                }
                else
                {
                    if (has_pick_fg)
                    {
                        params_changed = RenderAnslParamByKey("tool_colour", tool_engine, "pickFg", /*compact=*/true) || params_changed;
                        add_skip("pickFg");
                    }
                }

                ImGui::EndTable();
            }
        }
    }

    // Quick + sections (excluding reserved row).
    const AnslParamsUISkipList skip{ skip_keys.data(), (int)skip_keys.size() };
    params_changed = RenderAnslParamsUIPrimaryBar("tool_params_quick", tool_engine, &skip) || params_changed;
    ImGui::Separator();
    params_changed =
        RenderAnslParamsUIAdvanced("tool_params_sections", tool_engine, &skip, &session, compiled_tool_id) || params_changed;
    if (params_changed)
        tool_params::SaveToolParamsToSession(session, compiled_tool_id, tool_engine);

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return true;
}


