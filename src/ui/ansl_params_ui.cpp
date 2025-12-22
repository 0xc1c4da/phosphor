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
    // In compact mode, enforce consistent labels for common cross-tool toggles.
    // Many tools use verbose labels ("Fallback: Use FG") which makes the compact row inconsistent.
    const bool is_use_fg = (spec.key == "useFg");
    const bool is_use_bg = (spec.key == "useBg");
    const char* label =
        (compact && is_use_fg) ? "FG" :
        (compact && is_use_bg) ? "BG" :
        (spec.label.empty() ? spec.key.c_str() : spec.label.c_str());
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
            if (compact)
            {
                // Compact: force label on the left for consistent tool bars (avoid SliderInt label-on-right).
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                if (spec.width > 0.0f)
                    ImGui::SetNextItemWidth(spec.width);
                else
                    ImGui::SetNextItemWidth(180.0f);

                int v2 = v;
                const char* wid = "##int";
                bool edited = false;
                if (has_range && want_slider)
                    edited = ImGui::SliderInt(wid, &v2, spec.int_min, spec.int_max);
                else
                    edited = ImGui::DragInt(wid, &v2, (float)std::max(1, spec.int_step));

                if (edited)
                {
                    // Quantize to step.
                    const int step = std::max(1, spec.int_step);
                    if (has_range && step > 1)
                        v2 = spec.int_min + ((v2 - spec.int_min) / step) * step;
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
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
            if (compact)
            {
                // Compact: force label on the left for consistent tool bars (avoid SliderFloat label-on-right).
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                if (spec.width > 0.0f)
                    ImGui::SetNextItemWidth(spec.width);
                else
                    ImGui::SetNextItemWidth(180.0f);

                float v2 = v;
                const char* wid = "##float";
                bool edited = false;
                if (has_range && want_slider)
                    edited = ImGui::SliderFloat(wid, &v2, spec.float_min, spec.float_max);
                else
                    edited = ImGui::DragFloat(wid, &v2, spec.float_step);

                if (edited)
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
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

            // Fonts have huge enums; always prefer the searchable combo there.
            const bool want_filter_combo = (ui == "combo_filter") || (spec.key == "font");

            const bool want_segmented =
                !want_filter_combo && ((ui == "segmented") || (compact && ui != "combo" && (int)spec.enum_items.size() <= 6));

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
            else if (want_filter_combo)
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
                    // Filter input (make it obvious + auto-focus).
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Filter:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    char buf[256] = {};
                    if (!f.empty())
                        std::snprintf(buf, sizeof(buf), "%s", f.c_str());
                    if (ImGui::IsWindowAppearing())
                        ImGui::SetKeyboardFocusHere();
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

static bool IsSkippedKey(const AnslParamSpec& s, const AnslParamsUISkipList* skip)
{
    if (!skip || !skip->keys || skip->count <= 0)
        return false;
    for (int i = 0; i < skip->count; ++i)
    {
        const char* k = skip->keys[i];
        if (!k || !*k)
            continue;
        if (s.key == k)
            return true;
    }
    return false;
}

bool RenderAnslParamByKey(const char* id, AnslScriptEngine& engine, const char* key, bool compact)
{
    if (!key || !*key)
        return false;
    if (!engine.HasParams())
        return false;

    const auto& specs = engine.GetParamSpecs();
    const AnslParamSpec* found = nullptr;
    for (const auto& s : specs)
    {
        if (s.key == key)
        {
            found = &s;
            break;
        }
    }
    if (!found)
        return false;

    ImGui::PushID(id ? id : "ansl_param_by_key");
    ImGui::PushID(key);
    const bool changed = RenderParamControl(*found, engine, compact);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUIPrimaryBar(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params_primary";

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
        any_primary = any_primary || (s.primary && !IsSkippedKey(s, skip));

    if (any_primary)
    {
        bool have_prev_inline = false;
        for (const auto& s : specs)
        {
            if (!s.primary)
                continue;
            if (IsSkippedKey(s, skip))
                continue;

            if (have_prev_inline && s.inline_with_prev)
                ImGui::SameLine();
            ImGui::PushID(s.key.c_str());
            changed = RenderParamControl(s, engine, /*compact=*/true) || changed;
            ImGui::PopID();
            have_prev_inline = true;
        }
    }

    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUIAdvanced(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params_advanced";

    ImGui::PushID(id);
    bool changed = false;

    if (!engine.HasParams())
    {
        ImGui::TextDisabled("No parameters.");
        ImGui::PopID();
        return false;
    }

    const auto& specs = engine.GetParamSpecs();
    bool any_advanced = false;
    for (const auto& s : specs)
        any_advanced = any_advanced || (!s.primary && !IsSkippedKey(s, skip));

    if (!any_advanced)
    {
        ImGui::PopID();
        return false;
    }

            std::string cur_section;
            bool section_open = false;
            bool have_prev_inline = false;
            for (const auto& s : specs)
            {
                if (s.primary)
                    continue;
        if (IsSkippedKey(s, skip))
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

    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params";

    ImGui::PushID(id);
    bool changed = false;

    changed = RenderAnslParamsUIPrimaryBar("##primary", engine, skip) || changed;
    if (engine.HasParams())
    {
        // Only add a separator if advanced exists.
        bool any_advanced = false;
        for (const auto& s : engine.GetParamSpecs())
            any_advanced = any_advanced || (!s.primary && !IsSkippedKey(s, skip));
        if (any_advanced)
        {
            ImGui::Separator();
            changed = RenderAnslParamsUIAdvanced("##advanced", engine, skip) || changed;
        }
    }

    ImGui::PopID();
    return changed;
}


