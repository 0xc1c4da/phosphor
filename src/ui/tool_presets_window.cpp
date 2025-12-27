#include "ui/tool_presets_window.h"

#include "imgui.h"

#include "core/i18n.h"
#include "core/paths.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

ToolPresetsWindow::ToolPresetsWindow()
{
    path_ = PhosphorAssetPath("tool-presets.json");
}

void ToolPresetsWindow::EnsureLoaded()
{
    if (!loaded_)
        (void)Load();
}

bool ToolPresetsWindow::Load()
{
    presets_.clear();
    selected_slot_by_tool_.clear();
    last_error_.clear();

    std::string err;
    if (!tool_params::LoadToolParamPresetsFromFile(path_.c_str(), presets_, selected_slot_by_tool_, err))
    {
        last_error_ = err;
        loaded_ = true; // loaded (attempted), but errored
        dirty_ = false;
        return false;
    }

    loaded_ = true;
    dirty_ = false;
    return true;
}

bool ToolPresetsWindow::Save()
{
    last_error_.clear();
    std::string err;
    if (!tool_params::SaveToolParamPresetsToFile(path_.c_str(), presets_, selected_slot_by_tool_, err))
    {
        last_error_ = err;
        return false;
    }
    dirty_ = false;
    return true;
}

static std::string MakeUniqueTitleForToolAndSlot(const std::vector<tool_params::ToolParamPreset>& presets,
                                                const std::string& tool_id,
                                                int slot,
                                                std::string base)
{
    base.erase(0, base.find_first_not_of(" \t\r\n"));
    base.erase(base.find_last_not_of(" \t\r\n") + 1);
    if (base.empty())
        base = "Preset";

    auto exists = [&](const std::string& t) -> bool {
        for (const auto& p : presets)
            if (p.tool_id == tool_id && p.slot == slot && p.title == t)
                return true;
        return false;
    };

    if (!exists(base))
        return base;

    for (int i = 2; i < 999; ++i)
    {
        std::string t = base + " (" + std::to_string(i) + ")";
        if (!exists(t))
            return t;
    }
    return base;
}

bool ToolPresetsWindow::Render(const ToolSpec* active_tool,
                               const std::string& tool_id,
                               AnslScriptEngine& tool_engine,
                               SessionState& session,
                               bool* p_open,
                               bool apply_placement_this_frame)
{
    const char* base_id = "Tool Presets";
    const std::string title = std::string(PHOS_TR("menu.window.tool_presets")) + "###" + base_id;

    ApplyImGuiWindowPlacement(session, base_id, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session, base_id);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session, base_id);

    if (!ImGui::Begin(title.c_str(), p_open, flags))
    {
        CaptureImGuiWindowPlacement(session, base_id);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return (p_open == nullptr) ? true : *p_open;
    }

    CaptureImGuiWindowPlacement(session, base_id);
    ApplyImGuiWindowChromeZOrder(&session, base_id);
    RenderImGuiWindowChromeMenu(&session, base_id);

    EnsureLoaded();

    const std::string rename_popup =
        PHOS_TR("tool_presets_window.rename_modal_title") + "###tool_presets_rename_modal";

    // Title-bar kebab settings popup (file ops, etc.) similar to ToolPalette.
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##toolpresets_kebab", "\xE2\x8B\xAE", has_close, has_collapse, &kebab_min, &kebab_max))
            ImGui::OpenPopup("##toolpresets_menu");

        if (ImGui::IsPopupOpen("##toolpresets_menu"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(720.0f, 520.0f));
        if (ImGui::BeginPopup("##toolpresets_menu"))
        {
            const char* tool_label = (active_tool && !active_tool->label.empty()) ? active_tool->label.c_str() : PHOS_TR("common.none_parens").c_str();
            ImGui::TextUnformatted(tool_label);
            if (!tool_id.empty())
                ImGui::TextDisabled("%s", tool_id.c_str());
            ImGui::Separator();

            // File path
            ImGui::TextUnformatted(PHOS_TR("common.file").c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##tool_presets_path", &path_))
            {
                loaded_ = false;
                dirty_ = false;
                last_error_.clear();
            }

            if (ImGui::Button(PHOS_TR("common.reload").c_str()))
                (void)Load();
            ImGui::SameLine();
            if (ImGui::Button(PHOS_TR("common.save").c_str()))
                (void)Save();

            if (!last_error_.empty())
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());
            }

            ImGui::Separator();
            if (ImGui::Button(PHOS_TR("common.close").c_str()))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    auto find_preset_index_for_slot = [&](int slot) -> int {
        if (tool_id.empty())
            return -1;
        for (int i = 0; i < (int)presets_.size(); ++i)
        {
            const auto& p = presets_[(size_t)i];
            if (p.tool_id == tool_id && p.slot == slot)
                return i;
        }
        return -1;
    };

    const int selected_slot =
        (tool_id.empty() ? 0 : (selected_slot_by_tool_.count(tool_id) ? selected_slot_by_tool_[tool_id] : 0));

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Button sizing: prefer a single row of 1..9 when possible, otherwise wrap.
    const float min_w = 96.0f;
    const float min_h = 42.0f;
    int cols = 9;
    if (avail.x > 1.0f)
    {
        cols = (int)std::floor((avail.x + style.ItemSpacing.x) / (min_w + style.ItemSpacing.x));
        cols = std::clamp(cols, 1, 9);
    }
    const float total_spacing_x = style.ItemSpacing.x * (cols - 1);
    const float button_w = (cols > 0) ? std::max(min_w, (avail.x - total_spacing_x) / (float)cols) : min_w;
    const ImVec2 btn_sz(button_w, min_h);

    auto capture_current = [&]() -> std::unordered_map<std::string, SessionState::ToolParamValue> {
        std::unordered_map<std::string, SessionState::ToolParamValue> vals;
        (void)tool_params::CaptureToolParams(tool_engine, vals);
        return vals;
    };

    auto apply_preset = [&](const tool_params::ToolParamPreset& p) {
        if (tool_id.empty())
            return;
        tool_params::ApplyToolParams(p.values, tool_engine);
        tool_params::SaveToolParamsToSession(session, tool_id, tool_engine);
        if (p.slot >= 1 && p.slot <= 9)
            selected_slot_by_tool_[tool_id] = p.slot;
        (void)Save(); // best-effort; errors shown in UI
    };

    auto open_rename_modal = [&](int global_index) {
        rename_global_index_ = global_index;
        rename_new_title_.clear();
        rename_modal_open_ = true;
        ImGui::OpenPopup(rename_popup.c_str());
    };

    for (int slot = 1; slot <= 9; ++slot)
    {
        const int slot_index = slot - 1;
        if (slot_index % cols != 0)
            ImGui::SameLine();

        const int gi = find_preset_index_for_slot(slot);
        const bool has = (gi >= 0 && gi < (int)presets_.size());

        std::string label = std::to_string(slot);
        std::string title_txt = has ? presets_[(size_t)gi].title : PHOS_TR("common.empty_parens");
        const bool is_selected = has && (selected_slot == slot);

        ImGui::PushID(slot);

        // Button (like ToolPalette: button + draw overlay text).
        if (is_selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("##preset_btn", btn_sz))
        {
            if (has)
                apply_preset(presets_[(size_t)gi]);
            else
            {
                // Save current into this explicit slot.
                if (!tool_id.empty())
                {
                    tool_params::ToolParamPreset p;
                    p.tool_id = tool_id;
                    p.slot = slot;
                    p.title = MakeUniqueTitleForToolAndSlot(presets_, tool_id, slot, "Preset " + std::to_string(slot));
                    p.values = capture_current();
                    if (!p.values.empty())
                    {
                        presets_.push_back(std::move(p));
                        dirty_ = true;
                        (void)Save();
                    }
                }
            }
        }
        if (is_selected)
            ImGui::PopStyleColor();

        // Overlay: "N" + title (truncated visually by clip rect).
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        dl->PushClipRect(rmin, rmax, true);
        dl->AddText(ImVec2(rmin.x + 8.0f, rmin.y + 6.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
        dl->AddText(ImVec2(rmin.x + 26.0f, rmin.y + 6.0f), ImGui::GetColorU32(ImGuiCol_Text), title_txt.c_str());
        dl->PopClipRect();

        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(title_txt.c_str());
            if (has)
                ImGui::TextDisabled("Ctrl+%d", slot);
            ImGui::EndTooltip();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem("##ctx"))
        {
            if (has)
            {
                if (ImGui::MenuItem(PHOS_TR("tool_presets_window.ctx_apply").c_str()))
                    apply_preset(presets_[(size_t)gi]);
                if (ImGui::MenuItem(PHOS_TR("tool_presets_window.ctx_overwrite_with_current").c_str()))
                {
                    auto vals = capture_current();
                    if (!vals.empty())
                    {
                        presets_[(size_t)gi].values = std::move(vals);
                        dirty_ = true;
                        (void)Save();
                    }
                }
                if (ImGui::MenuItem(PHOS_TR("tool_presets_window.ctx_rename_ellipsis").c_str()))
                    open_rename_modal(gi);
                if (ImGui::MenuItem(PHOS_TR("common.delete").c_str()))
                {
                    const std::string old_title = presets_[(size_t)gi].title;
                    presets_.erase(presets_.begin() + gi);
                    if (!tool_id.empty() && selected_slot_by_tool_.count(tool_id) && selected_slot_by_tool_[tool_id] == slot)
                        selected_slot_by_tool_.erase(tool_id);
                    dirty_ = true;
                    (void)Save();
                }
            }
            else
            {
                if (ImGui::MenuItem(PHOS_TR("tool_presets_window.ctx_save_current_to_slot").c_str()))
                {
                    if (!tool_id.empty())
                    {
                        tool_params::ToolParamPreset p;
                        p.tool_id = tool_id;
                        p.slot = slot;
                        p.title = MakeUniqueTitleForToolAndSlot(presets_, tool_id, slot, "Preset " + std::to_string(slot));
                        p.values = capture_current();
                        if (!p.values.empty())
                        {
                            presets_.push_back(std::move(p));
                            dirty_ = true;
                            (void)Save();
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // Rename modal
    if (ImGui::BeginPopupModal(rename_popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (rename_global_index_ >= 0 && rename_global_index_ < (int)presets_.size())
        {
            const std::string old_title = presets_[(size_t)rename_global_index_].title;
            if (rename_new_title_.empty())
                rename_new_title_ = old_title;

            ImGui::TextUnformatted(PHOS_TR("tool_presets_window.rename_new_title_label").c_str());
            ImGui::SetNextItemWidth(360.0f);
            ImGui::InputText("##rename", &rename_new_title_);

            if (ImGui::Button(PHOS_TR("common.ok").c_str()))
            {
                if (!tool_id.empty())
                {
                    const int slot = presets_[(size_t)rename_global_index_].slot;
                    std::string new_title = MakeUniqueTitleForToolAndSlot(presets_, tool_id, slot, rename_new_title_);
                    presets_[(size_t)rename_global_index_].title = new_title;
                    dirty_ = true;
                    (void)Save();
                }
                rename_new_title_.clear();
                rename_global_index_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(PHOS_TR("common.cancel").c_str()))
            {
                rename_new_title_.clear();
                rename_global_index_ = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            ImGui::TextDisabled("%s", PHOS_TR("tool_presets_window.no_preset_selected").c_str());
            if (ImGui::Button(PHOS_TR("common.close").c_str()))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return (p_open == nullptr) ? true : *p_open;
}


