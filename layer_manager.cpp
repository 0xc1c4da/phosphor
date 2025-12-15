#include "layer_manager.h"

#include "canvas.h"
#include "imgui.h"

#include <cstdio>
#include <string>

void LayerManager::Render(const char* title,
                          bool* p_open,
                          const std::vector<LayerManagerCanvasRef>& canvases)
{
    if (!p_open || !*p_open)
        return;

    if (!ImGui::Begin(title, p_open, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    if (canvases.empty())
    {
        ImGui::TextUnformatted("No canvases open.");
        ImGui::End();
        return;
    }

    // Select target canvas by id.
    if (selected_canvas_id_ == 0)
        selected_canvas_id_ = canvases.front().id;

    std::vector<std::string> canvas_strings;
    std::vector<const char*> canvas_labels;
    canvas_strings.reserve(canvases.size());
    canvas_labels.reserve(canvases.size());
    for (const auto& c : canvases)
    {
        canvas_strings.push_back("Canvas " + std::to_string(c.id));
    }
    for (const std::string& s : canvas_strings)
        canvas_labels.push_back(s.c_str());

    int canvas_index = 0;
    bool canvas_id_found = false;
    for (size_t i = 0; i < canvases.size(); ++i)
    {
        if (canvases[i].id == selected_canvas_id_)
        {
            canvas_index = static_cast<int>(i);
            canvas_id_found = true;
            break;
        }
    }
    if (!canvas_id_found)
    {
        canvas_index = 0;
        selected_canvas_id_ = canvases.front().id;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("Target", &canvas_index, canvas_labels.data(), (int)canvas_labels.size()))
        selected_canvas_id_ = canvases[(size_t)canvas_index].id;

    AnsiCanvas* canvas = canvases[(size_t)canvas_index].canvas;
    if (!canvas)
    {
        ImGui::TextUnformatted("Selected canvas is null.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    const int layer_count = canvas->GetLayerCount();
    if (layer_count <= 0)
    {
        ImGui::TextUnformatted("Canvas has no layers (unexpected).");
        ImGui::End();
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
        bool target_alive = false;
        for (const auto& c : canvases)
        {
            if (c.canvas == rename_target_canvas_)
            {
                target_alive = true;
                break;
            }
        }

        if (!target_alive || !rename_target_canvas_)
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
            if (target_alive && rename_target_canvas_ && rename_target_layer_index_ >= 0)
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
}


