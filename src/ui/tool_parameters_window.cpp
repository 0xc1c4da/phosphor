#include "ui/tool_parameters_window.h"

#include "imgui.h"

#include "core/paths.h"
#include "io/session/imgui_persistence.h"
#include "ui/ansl_params_ui.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cstdio>

#include "misc/cpp/imgui_stdlib.h"

ToolParametersWindow::ToolParametersWindow()
{
    presets_path_ = PhosphorAssetPath("tool-presets.json");
}

// Helpers (declared early so they can be used by RenderPresetsPopup as well as Render()).
static const AnslParamSpec* FindParamSpec(const AnslScriptEngine& eng, const char* key);
static bool EngineHasPrimaryParamKey(const AnslScriptEngine& eng, const char* key);

void ToolParametersWindow::EnsurePresetsLoaded()
{
    if (presets_loaded_)
        return;
    std::string err;
    if (!tool_params::LoadToolParamPresetsFromFile(presets_path_.c_str(), presets_, selected_by_tool_, err))
        presets_error_ = err;
    else
        presets_error_.clear();
    presets_loaded_ = true;
}

void ToolParametersWindow::HandlePresetFileOps()
{
    if (request_reload_)
    {
        request_reload_ = false;
        std::string err;
        if (!tool_params::LoadToolParamPresetsFromFile(presets_path_.c_str(), presets_, selected_by_tool_, err))
            presets_error_ = err;
        else
            presets_error_.clear();
    }
    if (request_save_)
    {
        request_save_ = false;
        std::string err;
        if (!tool_params::SaveToolParamPresetsToFile(presets_path_.c_str(), presets_, selected_by_tool_, err))
            presets_error_ = err;
        else
            presets_error_.clear();
    }
}

static std::string TrimCopyLocal(const std::string& s)
{
    // Keep behavior consistent with other TrimCopy helpers in the codebase.
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

void ToolParametersWindow::RenderPresetsPopup(const char* base_id,
                                              const std::string& tool_id,
                                              AnslScriptEngine& tool_engine,
                                              SessionState& session,
                                              ImGuiWindowFlags flags)
{
    // Filter presets for current tool_id.
    std::vector<int> idxs;
    idxs.reserve(presets_.size());
    std::vector<const char*> titles;
    titles.reserve(presets_.size());
    for (int i = 0; i < (int)presets_.size(); ++i)
    {
        if (presets_[(size_t)i].tool_id == tool_id)
        {
            idxs.push_back(i);
            titles.push_back(presets_[(size_t)i].title.c_str());
        }
    }

    auto render_presets_panel = [&]() {
        // File
        ImGui::TextUnformatted("File");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##tool_param_presets_file", &presets_path_);
        if (!presets_error_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", presets_error_.c_str());
        if (ImGui::Button("Reload"))
            request_reload_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            request_save_ = true;

        ImGui::Separator();

        // Preset management for current tool id
        ImGui::TextUnformatted("Tool");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tool_id.empty() ? "(unknown)" : tool_id.c_str());

        int sel_local = 0;
        const auto it_sel = selected_by_tool_.find(tool_id);
        if (it_sel != selected_by_tool_.end() && !it_sel->second.empty())
        {
            for (int li = 0; li < (int)idxs.size(); ++li)
            {
                if (presets_[(size_t)idxs[(size_t)li]].title == it_sel->second)
                {
                    sel_local = li;
                    break;
                }
            }
        }

        tool_params::ToolParamPreset* cur = nullptr;
        if (!idxs.empty())
        {
            ImGui::TextUnformatted("Preset");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("##tool_param_preset_combo", &sel_local, titles.data(), (int)titles.size()))
            {
                const int gi = idxs[(size_t)sel_local];
                selected_by_tool_[tool_id] = presets_[(size_t)gi].title;
                request_save_ = true;
            }

            const int gi = idxs[(size_t)std::clamp(sel_local, 0, (int)idxs.size() - 1)];
            cur = (gi >= 0 && gi < (int)presets_.size()) ? &presets_[(size_t)gi] : nullptr;

            ImGui::SameLine();
            if (ImGui::Button("Overwrite selected") && cur)
            {
                (void)tool_params::CaptureToolParams(tool_engine, cur->values);
                request_save_ = true;
                tool_params::SaveToolParamsToSession(session, tool_id, tool_engine);
            }
        }
        else
        {
            ImGui::TextDisabled("(No presets for this tool yet)");
        }

        ImGui::Separator();

        if (ImGui::Button("Save current as…"))
            open_new_popup_ = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(idxs.empty() || !cur);
        if (ImGui::Button("Rename…"))
            open_rename_popup_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete…"))
            open_delete_popup_ = true;
        ImGui::EndDisabled();
    };

    // Title-bar ⋮ popup: Presets
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = false; // this window has no close button
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##tool_params_kebab", "\xE2\x8B\xAE", has_close, has_collapse, &kebab_min, &kebab_max))
            ImGui::OpenPopup("##tool_param_presets_popup");

        if (ImGui::IsPopupOpen("##tool_param_presets_popup"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(820.0f, 620.0f));
        if (ImGui::BeginPopup("##tool_param_presets_popup"))
        {
            ImGui::TextUnformatted("Presets");
            ImGui::Separator();
            render_presets_panel();
            ImGui::Separator();

            // Advanced tool params live here (formerly the "More…" section in the main window).
            ImGui::TextUnformatted("Advanced");
            ImGui::Separator();
            // Skip params that are deliberately surfaced in the main window's reserved rows
            // (e.g. color row). IMPORTANT: don't skip non-primary versions of these keys (e.g. Font fallback),
            // otherwise they'd disappear from Advanced; instead only skip if they are primary.
            std::vector<const char*> skip_keys;
            skip_keys.reserve(8);
            auto add_skip_primary = [&](const char* k) {
                if (EngineHasPrimaryParamKey(tool_engine, k))
                    skip_keys.push_back(k);
            };
            add_skip_primary("useFg");
            add_skip_primary("useBg");
            add_skip_primary("fgSource");
            add_skip_primary("bgSource");
            add_skip_primary("pickFg");
            add_skip_primary("pickBg");
            const AnslParamsUISkipList skip{ skip_keys.data(), (int)skip_keys.size() };
            (void)RenderAnslParamsUIAdvanced("tool_params_advanced_popup", tool_engine, &skip);

            ImGui::Separator();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Modal popups must be rendered from the Tool Parameters window (not inside the ⋮ popup),
    // otherwise they may disappear when the user closes the presets popup.

    // New preset popup
    if (open_new_popup_)
    {
        open_new_popup_ = false;
        std::snprintf(new_title_buf_, sizeof(new_title_buf_), "Preset");
        ImGui::OpenPopup("New Tool Param Preset");
    }
    if (ImGui::BeginPopupModal("New Tool Param Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save current parameter values as a preset.");
        ImGui::InputText("Title", new_title_buf_, IM_ARRAYSIZE(new_title_buf_));
        if (ImGui::Button("Create"))
        {
            tool_params::ToolParamPreset p;
            p.tool_id = tool_id;
            p.title = TrimCopyLocal(new_title_buf_);
            if (p.title.empty())
                p.title = "Untitled";
            (void)tool_params::CaptureToolParams(tool_engine, p.values);
            if (!p.tool_id.empty() && !p.values.empty())
            {
                presets_.push_back(std::move(p));
                selected_by_tool_[tool_id] = presets_.back().title;
                request_save_ = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Rename preset popup (current tool only)
    if (open_rename_popup_)
    {
        open_rename_popup_ = false;
        std::string cur_title;
        const auto it = selected_by_tool_.find(tool_id);
        if (it != selected_by_tool_.end())
            cur_title = it->second;
        std::snprintf(rename_title_buf_, sizeof(rename_title_buf_), "%s", cur_title.c_str());
        ImGui::OpenPopup("Rename Tool Param Preset");
    }
    if (ImGui::BeginPopupModal("Rename Tool Param Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Rename the selected preset.");
        ImGui::InputText("Title", rename_title_buf_, IM_ARRAYSIZE(rename_title_buf_));
        if (ImGui::Button("OK"))
        {
            if (!idxs.empty())
            {
                const std::string new_title = TrimCopyLocal(rename_title_buf_);
                if (!new_title.empty())
                {
                    // Find the currently selected preset by title.
                    const auto it = selected_by_tool_.find(tool_id);
                    const std::string cur_title = (it != selected_by_tool_.end()) ? it->second : std::string();
                    for (int gi : idxs)
                    {
                        if (gi >= 0 && gi < (int)presets_.size() && presets_[(size_t)gi].title == cur_title)
                        {
                            presets_[(size_t)gi].title = new_title;
                            selected_by_tool_[tool_id] = new_title;
                            request_save_ = true;
                            break;
                        }
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Delete preset popup (current tool only)
    if (open_delete_popup_)
    {
        open_delete_popup_ = false;
        ImGui::OpenPopup("Delete Tool Param Preset?");
    }
    if (ImGui::BeginPopupModal("Delete Tool Param Preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Delete the selected preset? This cannot be undone.");
        if (ImGui::Button("Delete"))
        {
            if (!idxs.empty())
            {
                const auto it = selected_by_tool_.find(tool_id);
                const std::string cur_title = (it != selected_by_tool_.end()) ? it->second : std::string();
                for (int k = (int)presets_.size() - 1; k >= 0; --k)
                {
                    if (presets_[(size_t)k].tool_id == tool_id && presets_[(size_t)k].title == cur_title)
                    {
                        presets_.erase(presets_.begin() + k);
                        request_save_ = true;
                        break;
                    }
                }
                selected_by_tool_.erase(tool_id);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Avoid unused warnings if caller doesn't use base_id in the future.
    (void)base_id;
}

static bool RenderPresetButtonsRow(const std::string& tool_id,
                                   std::vector<tool_params::ToolParamPreset>& presets,
                                   std::unordered_map<std::string, std::string>& selected_by_tool,
                                   AnslScriptEngine& tool_engine,
                                   SessionState& session,
                                   bool& request_save_selected)
{
    if (tool_id.empty())
        return false;

    // Collect presets for this tool.
    std::vector<int> idxs;
    idxs.reserve(presets.size());
    for (int i = 0; i < (int)presets.size(); ++i)
        if (presets[(size_t)i].tool_id == tool_id)
            idxs.push_back(i);

    if (idxs.empty())
        return false;

    const std::string selected_title =
        (selected_by_tool.find(tool_id) != selected_by_tool.end()) ? selected_by_tool[tool_id] : std::string();

    // Show a small set as buttons, overflow into a popup.
    constexpr int kMaxButtons = 6;
    int shown = 0;
    int overflow_start = std::min((int)idxs.size(), kMaxButtons);

    bool changed = false;
    for (int li = 0; li < overflow_start; ++li)
    {
        const int gi = idxs[(size_t)li];
        if (gi < 0 || gi >= (int)presets.size())
            continue;
        const auto& p = presets[(size_t)gi];

        const bool is_sel = (!selected_title.empty() && p.title == selected_title);
        if (is_sel)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        ImGui::PushID(gi);
        if (ImGui::SmallButton(p.title.c_str()))
        {
            selected_by_tool[tool_id] = p.title;
            request_save_selected = true;
            tool_params::ApplyToolParams(p.values, tool_engine);
            tool_params::SaveToolParamsToSession(session, tool_id, tool_engine);
            changed = true;
        }
        ImGui::PopID();

        if (is_sel)
            ImGui::PopStyleColor();

        if (li + 1 < overflow_start)
            ImGui::SameLine();
        shown++;
    }

    if ((int)idxs.size() > kMaxButtons)
    {
        if (overflow_start > 0)
            ImGui::SameLine();
        ImGui::PushID("preset_overflow");
        if (ImGui::SmallButton("…"))
            ImGui::OpenPopup("preset_overflow_popup");

        if (ImGui::BeginPopup("preset_overflow_popup"))
        {
            for (int li = kMaxButtons; li < (int)idxs.size(); ++li)
            {
                const int gi = idxs[(size_t)li];
                if (gi < 0 || gi >= (int)presets.size())
                    continue;
                const auto& p = presets[(size_t)gi];
                const bool is_sel = (!selected_title.empty() && p.title == selected_title);
                if (ImGui::MenuItem(p.title.c_str(), nullptr, is_sel))
                {
                    selected_by_tool[tool_id] = p.title;
                    request_save_selected = true;
                    tool_params::ApplyToolParams(p.values, tool_engine);
                    tool_params::SaveToolParamsToSession(session, tool_id, tool_engine);
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Trailing spacing cleanup: caller can decide whether to keep SameLine.
    (void)shown;
    return changed;
}

static bool EngineHasParamKey(const AnslScriptEngine& eng, const char* key)
{
    if (!key || !*key)
        return false;
    if (!eng.HasParams())
        return false;
    for (const auto& s : eng.GetParamSpecs())
        if (s.key == key)
            return true;
    return false;
}

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

static bool EngineHasPrimaryParamKey(const AnslScriptEngine& eng, const char* key)
{
    const AnslParamSpec* s = FindParamSpec(eng, key);
    return s && s->primary;
}

static bool AnyPresetsForTool(const std::vector<tool_params::ToolParamPreset>& presets, const std::string& tool_id)
{
    if (tool_id.empty())
        return false;
    for (const auto& p : presets)
        if (p.tool_id == tool_id)
            return true;
    return false;
}

bool ToolParametersWindow::Render(const ToolSpec* active_tool,
                                  const std::string& compiled_tool_id,
                                  AnslScriptEngine& tool_engine,
                                  SessionState& session,
                                  bool apply_placement_this_frame)
{
    if (!tool_engine.HasParams())
        return false;

    EnsurePresetsLoaded();
    HandlePresetFileOps();

    const char* base_id = "Tool Parameters";
    // Show tool label in the visible title, but keep a stable window ID for persistence.
    std::string wname = std::string(active_tool ? active_tool->label : "Tool Parameters") + "###" + base_id;

    ApplyImGuiWindowPlacement(session, base_id, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session, base_id);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session, base_id);

    ImGui::Begin(wname.c_str(), nullptr, flags);
    CaptureImGuiWindowPlacement(session, base_id);
    ApplyImGuiWindowChromeZOrder(&session, base_id);
    RenderImGuiWindowChromeMenu(&session, base_id);

    RenderPresetsPopup(base_id, compiled_tool_id, tool_engine, session, flags);

    bool params_changed = false;

    // Row 1 (reserved): Presets (first-class).
    {
        const bool has_presets = AnyPresetsForTool(presets_, compiled_tool_id);
        if (has_presets)
        {
            params_changed =
                RenderPresetButtonsRow(compiled_tool_id, presets_, selected_by_tool_, tool_engine, session, request_save_) || params_changed;
        }
        else
        {
            // Always provide an obvious entry point to create a preset, even when none exist yet.
            if (ImGui::SmallButton("+ Preset"))
                open_new_popup_ = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Save the current tool parameter values as a new preset.");
        }
    }
    ImGui::Separator();

    // Row 2 (reserved when present): Color row (FG/BG + related options like Source).
    // Only surface these here when the tool author marked them primary. Otherwise they remain in Advanced (e.g. Font fallback toggles).
    std::vector<const char*> skip_keys;
    skip_keys.reserve(8);
    auto add_skip = [&](const char* k) {
        if (k && *k)
            skip_keys.push_back(k);
    };

    {
        // Prefer the canonical "useFg/useBg" keys; fall back to pipette "pickFg/pickBg".
        const bool has_use_bg = EngineHasPrimaryParamKey(tool_engine, "useBg");
        const bool has_use_fg = EngineHasPrimaryParamKey(tool_engine, "useFg");
        const bool has_bg_src = EngineHasPrimaryParamKey(tool_engine, "bgSource");
        const bool has_fg_src = EngineHasPrimaryParamKey(tool_engine, "fgSource");

        const bool has_pick_bg = EngineHasPrimaryParamKey(tool_engine, "pickBg");
        const bool has_pick_fg = EngineHasPrimaryParamKey(tool_engine, "pickFg");

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

    // Main window shows only primary params (excluding reserved rows); advanced lives in ⋮.
    const AnslParamsUISkipList skip{ skip_keys.data(), (int)skip_keys.size() };
    params_changed = RenderAnslParamsUIPrimaryBar("tool_params_primary", tool_engine, &skip) || params_changed;
    if (params_changed)
        tool_params::SaveToolParamsToSession(session, compiled_tool_id, tool_engine);

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return true;
}


