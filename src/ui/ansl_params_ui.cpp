#include "ui/ansl_params_ui.h"

#include "imgui.h"

#include <algorithm>

static bool RenderParamRow(const AnslParamSpec& spec, AnslScriptEngine& engine)
{
    const char* label = spec.label.empty() ? spec.key.c_str() : spec.label.c_str();
    bool changed = false;

    switch (spec.type)
    {
        case AnslParamType::Bool:
        {
            bool v = false;
            if (!engine.GetParamBool(spec.key, v))
                return false;
            if (ImGui::Checkbox(label, &v))
            {
                engine.SetParamBool(spec.key, v);
                changed = true;
            }
            break;
        }
        case AnslParamType::Button:
        {
            if (ImGui::Button(label))
            {
                (void)engine.FireParamButton(spec.key);
                changed = true;
            }
            break;
        }
        case AnslParamType::Int:
        {
            int v = 0;
            if (!engine.GetParamInt(spec.key, v))
                return false;

            const bool has_range = (spec.int_min != spec.int_max);
            if (has_range)
            {
                int v2 = v;
                if (ImGui::SliderInt(label, &v2, spec.int_min, spec.int_max))
                {
                    // Quantize to step.
                    const int step = std::max(1, spec.int_step);
                    if (step > 1)
                        v2 = spec.int_min + ((v2 - spec.int_min) / step) * step;
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                int v2 = v;
                if (ImGui::DragInt(label, &v2, (float)std::max(1, spec.int_step)))
                {
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                }
            }
            break;
        }
        case AnslParamType::Float:
        {
            float v = 0.0f;
            if (!engine.GetParamFloat(spec.key, v))
                return false;

            const bool has_range = (spec.float_min != spec.float_max);
            if (has_range)
            {
                float v2 = v;
                if (ImGui::SliderFloat(label, &v2, spec.float_min, spec.float_max))
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                float v2 = v;
                if (ImGui::DragFloat(label, &v2, spec.float_step))
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                }
            }
            break;
        }
        case AnslParamType::Enum:
        {
            std::string cur;
            if (!engine.GetParamEnum(spec.key, cur))
                return false;
            if (spec.enum_items.empty())
                return false;

            int cur_idx = 0;
            for (int i = 0; i < (int)spec.enum_items.size(); ++i)
            {
                if (spec.enum_items[(size_t)i] == cur)
                {
                    cur_idx = i;
                    break;
                }
            }

            // Build a stable char* array for ImGui::Combo.
            // Note: pointers reference strings owned by spec (stable for this frame).
            std::vector<const char*> items;
            items.reserve(spec.enum_items.size());
            for (const auto& s : spec.enum_items)
                items.push_back(s.c_str());

            int idx2 = cur_idx;
            if (ImGui::Combo(label, &idx2, items.data(), (int)items.size()))
            {
                idx2 = std::clamp(idx2, 0, (int)spec.enum_items.size() - 1);
                engine.SetParamEnum(spec.key, spec.enum_items[(size_t)idx2]);
                changed = true;
            }
            break;
        }
    }

    return changed;
}

bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine)
{
    if (!id)
        id = "ansl_params";

    ImGui::PushID(id);
    bool changed = false;

    if (!engine.HasParams())
    {
        ImGui::TextDisabled("No parameters.");
        ImGui::PopID();
        return false;
    }

    if (ImGui::Button("Reset"))
    {
        engine.ResetParamsToDefaults();
        changed = true;
    }

    ImGui::Separator();

    const auto& specs = engine.GetParamSpecs();
    bool have_prev = false;
    for (const auto& s : specs)
    {
        if (have_prev && s.same_line)
            ImGui::SameLine();
        ImGui::PushID(s.key.c_str());
        changed = RenderParamRow(s, engine) || changed;
        ImGui::PopID();
        have_prev = true;
    }

    ImGui::PopID();
    return changed;
}


