#include "layer_manager.h"

#include "canvas.h"
#include "imgui.h"

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
    for (size_t i = 0; i < canvases.size(); ++i)
    {
        if (canvases[i].id == selected_canvas_id_)
        {
            canvas_index = static_cast<int>(i);
            break;
        }
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

    ImGui::Separator();

    if (ImGui::Button("+ Add Layer"))
        canvas->AddLayer("");
    ImGui::SameLine();
    if (ImGui::Button("- Remove Layer"))
        canvas->RemoveLayer(canvas->GetActiveLayerIndex());

    ImGui::End();
}


