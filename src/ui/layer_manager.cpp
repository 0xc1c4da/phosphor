#include "ui/layer_manager.h"

#include "core/canvas.h"
#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <cstdio>
#include <string>

void LayerManager::Render(const char* title,
                          bool* p_open,
                          AnsiCanvas* active_canvas,
                          SessionState* session,
                          bool apply_placement_this_frame)
{
    if (!p_open || !*p_open)
        return;

    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    if (!ImGui::Begin(title, p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, title);
        RenderImGuiWindowChromeMenu(session, title);
    }

    if (!active_canvas)
    {
        ImGui::TextUnformatted("No active canvas.");
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }

    AnsiCanvas* canvas = active_canvas;

    ImGui::Separator();

    const int layer_count = canvas->GetLayerCount();
    if (layer_count <= 0)
    {
        ImGui::TextUnformatted("Canvas has no layers (unexpected).");
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }

    // Build layer labels for combo.
    std::vector<std::string> layer_strings;
    std::vector<const char*> layer_labels;
    layer_strings.reserve((size_t)layer_count);
    layer_labels.reserve((size_t)layer_count);
    for (int i = 0; i < layer_count; ++i)
    {
        std::string name = canvas->GetLayerName(i);
        if (name.empty())
            name = "(unnamed)";
        layer_strings.push_back(std::to_string(i) + ": " + name);
    }
    for (const std::string& s : layer_strings)
        layer_labels.push_back(s.c_str());

    int active = canvas->GetActiveLayerIndex();
    if (active < 0) active = 0;
    if (active >= layer_count) active = layer_count - 1;

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("Active Layer", &active, layer_labels.data(), layer_count))
        canvas->SetActiveLayerIndex(active);

    bool vis = canvas->IsLayerVisible(canvas->GetActiveLayerIndex());
    if (ImGui::Checkbox("Visible", &vis))
        canvas->SetLayerVisible(canvas->GetActiveLayerIndex(), vis);

    ImGui::SameLine();
    if (ImGui::Button("Rename..."))
    {
        rename_target_canvas_ = canvas;
        rename_target_layer_index_ = canvas->GetActiveLayerIndex();

        const std::string current = canvas->GetLayerName(rename_target_layer_index_);
        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", current.c_str());

        // Use a stable popup name but a unique ID scope per invocation.
        // This avoids ID mismatches between OpenPopup() and BeginPopupModal().
        rename_popup_serial_++;
        rename_popup_active_serial_ = rename_popup_serial_;
        rename_popup_requested_open_ = true;
    }

    // Open the popup when requested (must be done in the same ID scope as BeginPopupModal).
    if (rename_popup_requested_open_)
    {
        ImGui::PushID(rename_popup_active_serial_);
        ImGui::OpenPopup("Rename Layer");
        ImGui::PopID();
        rename_popup_requested_open_ = false;
    }

    // Always try to render the modal for the active rename serial; if it's not open, BeginPopupModal returns false.
    ImGui::PushID(rename_popup_active_serial_);
    if (ImGui::BeginPopupModal("Rename Layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Verify the target canvas still exists this frame (avoid dangling pointer).
        const bool target_alive = (rename_target_canvas_ != nullptr) && (rename_target_canvas_ == active_canvas);
        if (!target_alive)
        {
            ImGui::TextUnformatted("Target canvas no longer exists.");
        }
        else
        {
            ImGui::Text("Layer %d name:", rename_target_layer_index_);
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##rename_layer_name", rename_buf_, IM_ARRAYSIZE(rename_buf_));
        }

        if (ImGui::Button("OK"))
        {
            if (target_alive && rename_target_layer_index_ >= 0)
                rename_target_canvas_->SetLayerName(rename_target_layer_index_, std::string(rename_buf_));
            rename_target_canvas_ = nullptr;
            rename_target_layer_index_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            rename_target_canvas_ = nullptr;
            rename_target_layer_index_ = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::PopID();

    // Reorder active layer within the stack.
    const int active_layer = canvas->GetActiveLayerIndex();
    const bool can_move_down = (active_layer > 0);
    const bool can_move_up   = (active_layer >= 0 && active_layer < layer_count - 1);

    if (!can_move_down) ImGui::BeginDisabled();
    if (ImGui::Button("Move Down"))
        canvas->MoveLayerDown(active_layer);
    if (!can_move_down) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!can_move_up) ImGui::BeginDisabled();
    if (ImGui::Button("Move Up"))
        canvas->MoveLayerUp(active_layer);
    if (!can_move_up) ImGui::EndDisabled();

    ImGui::Separator();

    if (ImGui::Button("+ Add Layer"))
        canvas->AddLayer("");
    ImGui::SameLine();
    if (ImGui::Button("- Remove Layer"))
        canvas->RemoveLayer(canvas->GetActiveLayerIndex());

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}


