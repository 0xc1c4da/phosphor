#include "ui/tool_presets_window.h"

#include "app/focus_router.h"
#include "imgui.h"
#include "imgui_internal.h"

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

void ToolPresetsWindow::NotifySelectedSlot(const std::string& tool_id, int slot)
{
    if (tool_id.empty())
        return;
    if (slot < 1 || slot > 9)
        return;
    selected_slot_by_tool_[tool_id] = slot;
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
                               bool apply_placement_this_frame,
                               app::FocusRouter* focus_router)
{
    const char* base_id = "Tool Presets";
    // Show tool label in the visible title, but keep a stable window ID for persistence.
    const std::string presets_word = PHOS_TR("tool_parameters.presets_popup_title"); // "Presets"
    const std::string fallback_title = PHOS_TR("menu.window.tool_presets");
    const std::string visible_title =
        (active_tool && !active_tool->label.empty()) ? (std::string(active_tool->label) + " " + presets_word) : fallback_title;
    const std::string title = visible_title + "###" + base_id;

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
    const bool window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focus_router)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        ImGuiWindow* root = (w && w->RootWindow) ? w->RootWindow : w;
        const std::uint32_t root_id = root ? (std::uint32_t)root->ID : 0u;
        focus_router->NoteWindowTarget(app::TargetKind::ToolPresets, root_id, window_focused);
    }

    CaptureImGuiWindowPlacement(session, base_id);
    ApplyImGuiWindowChromeZOrder(&session, base_id);
    RenderImGuiWindowChromeMenu(&session, base_id);

    // Transitional key ownership: this window is keyboard-navigable via ImGui nav.
    // Lock arrows/Enter/Escape when focused and not editing a widget so keys don't leak into the canvas.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetActiveID() == 0 &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        const ImGuiID owner = ImGui::GetCurrentWindow()->ID;
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Enter, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Escape, owner, ImGuiInputFlags_LockThisFrame);
    }

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

    // Put the buttons in a scrollable child so tiny windows never "lose" slots off-screen.
    // Also reserve scrollbar width deterministically to avoid wrapping a button under it.
    ImGui::BeginChild("##tool_presets_slots",
                      /*size=*/ImVec2(0.0f, 0.0f),
                      /*border=*/false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Preset slot buttons: variable width sized to title (like a wrapping "chip" layout).
    // Height should track the current UI font size (Unscii) so the control stays crisp
    // and consistent with the rest of the UI.
    const float button_h = std::floor(ImGui::GetFrameHeight() + 0.5f);
    const float min_button_w = 56.0f;
    const float left_pad = 8.0f;   // space before slot number
    const float mid_gap = 10.0f;   // space between slot number and title
    const float right_pad = 10.0f; // space after title
    const float avail_w = std::max(1.0f, avail.x);
    float row_used = 0.0f; // in content coords

    // Stepped font sizes for titles (max = current font size). We do two things:
    // - A *global* step based on available width (so resizing actually changes typography).
    // - A *per-title* fallback step only if the title still doesn't fit.
    //
    // Using discrete steps avoids "jitter" during resize.
    const float base_font_size = ImGui::GetFontSize();
    ImFont* font = ImGui::GetFont();

    // Pixel-aligned font size steps (for crisp bitmap-style UI fonts like Unscii 8x16).
    //
    // ImGui scales glyphs from the baked atlas; arbitrary fractional scaling produces blur.
    // For monospaced pixel fonts, we prefer snapping so the *rendered glyph cell width* is an
    // integer number of pixels (similar to AnsiCanvas zoom snapping).
    //
    // We derive steps from the base glyph cell width in pixels (at the current font size),
    // and quantize to integer cell widths down to 1px.
    const float base_cell_w_px = std::max(1.0f, ImGui::CalcTextSize("M").x);
    std::vector<float> title_font_sizes;
    title_font_sizes.reserve(16);
    if (font && base_cell_w_px > 0.0f && base_font_size > 0.0f)
    {
        const int max_cell_w = std::clamp((int)std::floor(base_cell_w_px + 0.5f), 1, 256);
        for (int cell_w = max_cell_w; cell_w >= 1; --cell_w)
        {
            const float scale = (float)cell_w / base_cell_w_px;
            const float fs = std::max(1.0f, std::floor(base_font_size * scale + 0.5f));
            if (title_font_sizes.empty() || std::fabs(title_font_sizes.back() - fs) > 0.01f)
                title_font_sizes.push_back(fs);
        }
    }
    if (title_font_sizes.empty())
        title_font_sizes.push_back(std::max(1.0f, base_font_size));
    // Only allow a small number of snap steps; if the title still doesn't fit, we'll clip it.
    // This preserves the pixel-font look (avoid ultra-tiny blurry text).
    const int kMaxTitleSteps = 6;
    if ((int)title_font_sizes.size() > kMaxTitleSteps)
        title_font_sizes.resize((size_t)kMaxTitleSteps);

    // Choose the global step by simulating the wrapped layout for each scale and picking
    // the largest one that fits in BOTH available width (wrap) and height (rows visible).
    int global_title_step = 0; // index into title_font_sizes (0 = largest)
    if (font && avail_w > 1.0f && avail.y > 1.0f)
    {
        auto title_text_for_slot = [&](int slot) -> std::string {
            const int gi = find_preset_index_for_slot(slot);
            const bool has = (gi >= 0 && gi < (int)presets_.size());
            return has ? presets_[(size_t)gi].title : PHOS_TR("common.empty");
        };

        auto simulate_total_height_for_step = [&](int step) -> float {
            const float title_fs = title_font_sizes[(size_t)std::clamp(step, 0, (int)title_font_sizes.size() - 1)];
            float row_used_sim = 0.0f;
            int rows = 1;
            for (int slot = 1; slot <= 9; ++slot)
            {
                const std::string label = std::to_string(slot);
                const std::string title_txt = title_text_for_slot(slot);

                const ImVec2 label_sz = font->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, label.c_str());
                const ImVec2 title_sz = font->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, title_txt.c_str());
                const float title_x = left_pad + label_sz.x + mid_gap;
                float btn_w = std::max(min_button_w, title_x + title_sz.x + right_pad + style.FramePadding.x * 2.0f);
                btn_w = std::clamp(btn_w, min_button_w, avail_w);

                if (row_used_sim > 0.0f)
                {
                    const float need = style.ItemSpacing.x + btn_w;
                    if (row_used_sim + need > avail_w)
                    {
                        row_used_sim = 0.0f;
                        rows++;
                    }
                    else
                    {
                        row_used_sim += style.ItemSpacing.x;
                    }
                }
                row_used_sim += btn_w;
            }

            return (float)rows * button_h + (float)std::max(0, rows - 1) * style.ItemSpacing.y;
        };

        int best_fit_step = -1;
        float best_overflow = FLT_MAX;
        for (int step = 0; step < (int)title_font_sizes.size(); ++step)
        {
            const float total_h = simulate_total_height_for_step(step);
            const float overflow = std::max(0.0f, total_h - avail.y);
            if (overflow <= 0.0f)
            {
                best_fit_step = step;
                break; // first fit is the largest font (steps are ordered largest->smallest)
            }
            if (overflow < best_overflow)
            {
                best_overflow = overflow;
                global_title_step = step; // best so far (min overflow) in case none fit
            }
        }
        if (best_fit_step >= 0)
            global_title_step = best_fit_step;
    }

    auto pick_title_font_size = [&](const std::string& text, float max_w) -> float {
        if (!font || text.empty() || !(max_w > 1.0f) || title_font_sizes.empty())
            return title_font_sizes.empty() ? std::max(1.0f, base_font_size) : title_font_sizes[(size_t)std::clamp(global_title_step, 0, (int)title_font_sizes.size() - 1)];
        for (int i = std::clamp(global_title_step, 0, (int)title_font_sizes.size() - 1); i < (int)title_font_sizes.size(); ++i)
        {
            const float fs = title_font_sizes[(size_t)i];
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
            if (ts.x <= max_w)
                return fs;
        }
        return title_font_sizes.back();
    };

    const float global_text_font_size =
        title_font_sizes.empty() ? std::max(1.0f, base_font_size)
                                 : title_font_sizes[(size_t)std::clamp(global_title_step, 0, (int)title_font_sizes.size() - 1)];

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
        const int gi = find_preset_index_for_slot(slot);
        const bool has = (gi >= 0 && gi < (int)presets_.size());

        std::string label = std::to_string(slot);
        std::string title_txt = has ? presets_[(size_t)gi].title : PHOS_TR("common.empty");
        const bool is_selected = (!tool_id.empty() && selected_slot == slot);

        // Compute variable width based on label + title.
        const float title_font_base = global_text_font_size;
        const ImVec2 label_sz =
            font ? font->CalcTextSizeA(title_font_base, FLT_MAX, 0.0f, label.c_str())
                 : ImGui::CalcTextSize(label.c_str());
        const ImVec2 title_sz = font ? font->CalcTextSizeA(title_font_base, FLT_MAX, 0.0f, title_txt.c_str())
                                     : ImGui::CalcTextSize(title_txt.c_str());
        const float title_x = left_pad + label_sz.x + mid_gap;
        float btn_w = std::max(min_button_w, title_x + title_sz.x + right_pad + style.FramePadding.x * 2.0f);
        btn_w = std::clamp(btn_w, min_button_w, avail_w);

        // Wrap to next row if needed.
        if (row_used > 0.0f)
        {
            const float need = style.ItemSpacing.x + btn_w;
            if (row_used + need > avail_w)
                row_used = 0.0f;
            else
            {
                ImGui::SameLine();
                row_used += style.ItemSpacing.x;
            }
        }
        row_used += btn_w;

        ImGui::PushID(slot);

        // Button (like ToolPalette: button + draw overlay text).
        if (is_selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("##preset_btn", ImVec2(btn_w, button_h)))
        {
            // Always select the slot on click so the highlight matches user intent.
            if (!tool_id.empty() && (selected_slot_by_tool_.count(tool_id) == 0 || selected_slot_by_tool_[tool_id] != slot))
            {
                selected_slot_by_tool_[tool_id] = slot;
                (void)Save(); // best-effort; errors shown in UI
            }

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
                        selected_slot_by_tool_[tool_id] = slot;
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
        // Vertically center text inside the button using snapped global text size.
        const float text_y = std::floor(rmin.y + std::max(0.0f, (button_h - global_text_font_size) * 0.5f));
        const ImVec2 label_pos(std::floor(rmin.x + left_pad), text_y);
        const ImVec2 title_pos(std::floor(rmin.x + title_x), text_y);
        const float max_title_w = std::max(1.0f, (rmax.x - right_pad) - title_pos.x);
        const float title_font_size = pick_title_font_size(title_txt, max_title_w);

        dl->AddText(font, global_text_font_size, label_pos,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    label.c_str());
        dl->AddText(font, title_font_size, title_pos,
                    ImGui::GetColorU32(has ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    title_txt.c_str(),
                    /*text_end=*/nullptr,
                    /*wrap_width=*/0.0f);
        dl->PopClipRect();

        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            if (has)
                ImGui::TextUnformatted(title_txt.c_str());
            else
                ImGui::TextUnformatted(PHOS_TR("common.empty_parens").c_str());
            if (has)
                ImGui::TextDisabled("Ctrl+%d", slot);
            else
                ImGui::TextDisabled("%s", PHOS_TR("tool_presets_window.ctx_save_current_to_slot").c_str());
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
                            selected_slot_by_tool_[tool_id] = slot;
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

    ImGui::EndChild();

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


