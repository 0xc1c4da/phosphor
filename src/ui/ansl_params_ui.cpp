#include "ui/ansl_params_ui.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool StrIContains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    const std::string h = ToLower(haystack);
    const std::string n = ToLower(needle);
    return h.find(n) != std::string::npos;
}

static bool ToggleButton(const char* label, bool v, const ImVec2& size = ImVec2(0, 0))
{
    ImVec4 col = ImGui::GetStyleColorVec4(v ? ImGuiCol_ButtonActive : ImGuiCol_Button);
    ImVec4 hov = ImGui::GetStyleColorVec4(v ? ImGuiCol_ButtonHovered : ImGuiCol_ButtonHovered);
    ImVec4 act = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

static bool RenderEnumSegmented(const char* label,
                               const AnslParamSpec& spec,
                               const std::string& cur,
                               int& out_idx)
{
    if (spec.enum_items.empty())
        return false;

    if (label && *label)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
    }

    bool changed = false;
    out_idx = 0;
    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
    {
        if (spec.enum_items[(size_t)i] == cur)
            out_idx = i;
    }

    ImGui::BeginGroup();
    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
    {
        if (i != 0)
            ImGui::SameLine();
        const bool selected = (i == out_idx);
        ImGui::PushID(i);
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(spec.enum_items[(size_t)i].c_str()))
        {
            out_idx = i;
            changed = true;
        }
        if (selected)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndGroup();
    return changed;
}

static bool RenderParamControl(const AnslParamSpec& spec, AnslScriptEngine& engine, bool compact)
{
    const char* label = spec.label.empty() ? spec.key.c_str() : spec.label.c_str();
    const std::string ui = ToLower(spec.ui);
    bool changed = false;

    // Optional enablement condition (bool param gate).
    bool enabled = true;
    if (!spec.enabled_if.empty())
    {
        bool gate = false;
        if (engine.GetParamBool(spec.enabled_if, gate))
            enabled = gate;
    }
    if (!enabled)
        ImGui::BeginDisabled(true);

    if (spec.width > 0.0f)
        ImGui::SetNextItemWidth(spec.width);

    switch (spec.type)
    {
        case AnslParamType::Bool:
        {
            bool v = false;
            if (!engine.GetParamBool(spec.key, v))
                return false;

            const bool want_toggle = (ui == "toggle") || (compact && ui != "checkbox");
            if (want_toggle)
            {
                if (ToggleButton(label, v))
                {
                    engine.SetParamBool(spec.key, !v);
                    changed = true;
                }
            }
            else
            {
                if (ImGui::Checkbox(label, &v))
                {
                    engine.SetParamBool(spec.key, v);
                    changed = true;
                }
            }
            break;
        }
        case AnslParamType::Button:
        {
            // Buttons are actions: render as normal buttons, but allow compact mode styling.
            if (compact)
            {
                if (ImGui::SmallButton(label))
                {
                    (void)engine.FireParamButton(spec.key);
                    changed = true;
                }
            }
            else if (ImGui::Button(label))
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
            const bool want_slider = (ui == "slider") || (has_range && ui != "drag");
            if (has_range && want_slider)
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
            const bool want_slider = (ui == "slider") || (has_range && ui != "drag");
            if (has_range && want_slider)
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

            const bool want_segmented =
                (ui == "segmented") || (compact && ui != "combo" && (int)spec.enum_items.size() <= 6);

            if (want_segmented)
            {
                int idx2 = 0;
                if (RenderEnumSegmented(label, spec, cur, idx2))
                {
                    idx2 = std::clamp(idx2, 0, (int)spec.enum_items.size() - 1);
                    engine.SetParamEnum(spec.key, spec.enum_items[(size_t)idx2]);
                    changed = true;
                }
            }
            else if (ui == "combo_filter")
            {
                // Combo with inline filter field (useful for large enums like fonts).
                // Keep filter state per-widget ID.
                static std::unordered_map<ImGuiID, std::string> s_enum_filters;
                ImGui::PushID("combo_filter");
                const ImGuiID fid = ImGui::GetID(spec.key.c_str());
                std::string& f = s_enum_filters[fid];

                const char* preview = cur.c_str();
                if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge))
                {
                    // Filter input
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    char buf[256] = {};
                    if (!f.empty())
                        std::snprintf(buf, sizeof(buf), "%s", f.c_str());
                    if (ImGui::InputTextWithHint("##filter", "type to filter…", buf, sizeof(buf)))
                        f = buf;
                    ImGui::Separator();

                    int picked_idx = -1;
                    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
                    {
                        const std::string& item = spec.enum_items[(size_t)i];
                        if (!f.empty() && !StrIContains(item, f))
                            continue;
                        const bool is_sel = (item == cur);
                        if (ImGui::Selectable(item.c_str(), is_sel))
                            picked_idx = i;
                        if (is_sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    if (picked_idx >= 0)
                    {
                        engine.SetParamEnum(spec.key, spec.enum_items[(size_t)picked_idx]);
                        changed = true;
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            else
            {
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
            }
            break;
        }
    }

    if (!spec.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", spec.tooltip.c_str());

    if (!enabled)
        ImGui::EndDisabled();

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

    const auto& specs = engine.GetParamSpecs();
    // Primary bar (compact): show "primary" params first.
    bool any_primary = false;
    for (const auto& s : specs)
        any_primary = any_primary || s.primary;

    if (any_primary)
    {
        bool have_prev_inline = false;
        for (const auto& s : specs)
        {
            if (!s.primary)
                continue;

            if (have_prev_inline && s.inline_with_prev)
                ImGui::SameLine();
            ImGui::PushID(s.key.c_str());
            changed = RenderParamControl(s, engine, /*compact=*/true) || changed;
            ImGui::PopID();
            have_prev_inline = true;
        }
    }

    // Advanced params live under a single collapsible "More…" section to keep the window compact.
    bool any_advanced = false;
    for (const auto& s : specs)
        any_advanced = any_advanced || !s.primary;

    if (any_advanced)
    {
        if (any_primary)
            ImGui::Separator();

        const ImGuiTreeNodeFlags more_flags = any_primary ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;
        if (ImGui::CollapsingHeader("More…", more_flags))
        {
            std::string cur_section;
            bool section_open = false;
            bool have_prev_inline = false;
            for (const auto& s : specs)
            {
                if (s.primary)
                    continue;

                const std::string sec = s.section.empty() ? "General" : s.section;
                if (sec != cur_section)
                {
                    cur_section = sec;
                    have_prev_inline = false;
                    section_open = ImGui::CollapsingHeader(cur_section.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!section_open)
                    continue;

                if (have_prev_inline && s.inline_with_prev)
                    ImGui::SameLine();

                ImGui::PushID(s.key.c_str());
                changed = RenderParamControl(s, engine, /*compact=*/false) || changed;
                ImGui::PopID();
                have_prev_inline = true;
            }
        }
    }

    ImGui::PopID();
    return changed;
}


