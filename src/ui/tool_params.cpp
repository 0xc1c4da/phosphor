#include "ui/tool_params.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace tool_params
{
using nlohmann::json;

static std::string TrimCopy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

void SaveToolParamsToSession(SessionState& session, const std::string& tool_id, const AnslScriptEngine& eng)
{
    if (tool_id.empty())
        return;
    if (!eng.HasParams())
        return;

    std::unordered_map<std::string, SessionState::ToolParamValue> out;
    const auto& specs = eng.GetParamSpecs();
    out.reserve(specs.size());

    for (const auto& spec : specs)
    {
        if (spec.key.empty())
            continue;

        SessionState::ToolParamValue v;
        v.type = (int)spec.type;

        switch (spec.type)
        {
            case AnslParamType::Bool:
            {
                bool b = false;
                if (!eng.GetParamBool(spec.key, b))
                    continue;
                v.b = b;
            } break;
            case AnslParamType::Int:
            {
                int i = 0;
                if (!eng.GetParamInt(spec.key, i))
                    continue;
                v.i = i;
            } break;
            case AnslParamType::Float:
            {
                float f = 0.0f;
                if (!eng.GetParamFloat(spec.key, f))
                    continue;
                v.f = f;
            } break;
            case AnslParamType::Enum:
            {
                std::string s;
                if (!eng.GetParamEnum(spec.key, s))
                    continue;
                v.s = std::move(s);
            } break;
            case AnslParamType::Button:
            {
                // Edge-triggered; never persist "pressed" state.
                v.b = false;
            } break;
        }

        out[spec.key] = std::move(v);
    }

    if (!out.empty())
        session.tool_param_values[tool_id] = std::move(out);
}

void RestoreToolParamsFromSession(const SessionState& session, const std::string& tool_id, AnslScriptEngine& eng)
{
    if (tool_id.empty())
        return;
    if (!eng.HasParams())
        return;

    const auto it_tool = session.tool_param_values.find(tool_id);
    if (it_tool == session.tool_param_values.end())
        return;
    const auto& saved = it_tool->second;
    if (saved.empty())
        return;

    auto enum_value_valid = [](const AnslParamSpec& spec, const std::string& v) -> bool {
        if (spec.type != AnslParamType::Enum)
            return true;
        for (const std::string& item : spec.enum_items)
            if (item == v)
                return true;
        return false;
    };

    for (const auto& spec : eng.GetParamSpecs())
    {
        if (spec.key.empty())
            continue;
        const auto it = saved.find(spec.key);
        if (it == saved.end())
            continue;
        const SessionState::ToolParamValue& v = it->second;
        if (v.type != (int)spec.type)
            continue;

        switch (spec.type)
        {
            case AnslParamType::Bool:  (void)eng.SetParamBool(spec.key, v.b); break;
            case AnslParamType::Int:   (void)eng.SetParamInt(spec.key, v.i); break;
            case AnslParamType::Float: (void)eng.SetParamFloat(spec.key, v.f); break;
            case AnslParamType::Enum:
            {
                if (enum_value_valid(spec, v.s))
                    (void)eng.SetParamEnum(spec.key, v.s);
            } break;
            case AnslParamType::Button:
                break;
        }
    }
}

bool CaptureToolParams(const AnslScriptEngine& eng,
                       std::unordered_map<std::string, SessionState::ToolParamValue>& out)
{
    out.clear();
    if (!eng.HasParams())
        return false;

    const auto& specs = eng.GetParamSpecs();
    out.reserve(specs.size());

    for (const auto& spec : specs)
    {
        if (spec.key.empty())
            continue;

        SessionState::ToolParamValue v;
        v.type = (int)spec.type;

        switch (spec.type)
        {
            case AnslParamType::Bool:
            {
                bool b = false;
                if (!eng.GetParamBool(spec.key, b))
                    continue;
                v.b = b;
            } break;
            case AnslParamType::Int:
            {
                int i = 0;
                if (!eng.GetParamInt(spec.key, i))
                    continue;
                v.i = i;
            } break;
            case AnslParamType::Float:
            {
                float f = 0.0f;
                if (!eng.GetParamFloat(spec.key, f))
                    continue;
                v.f = f;
            } break;
            case AnslParamType::Enum:
            {
                std::string s;
                if (!eng.GetParamEnum(spec.key, s))
                    continue;
                v.s = std::move(s);
            } break;
            case AnslParamType::Button:
            {
                // Edge-triggered; never persist "pressed" state.
                v.b = false;
            } break;
        }

        out[spec.key] = std::move(v);
    }

    return !out.empty();
}

void ApplyToolParams(const std::unordered_map<std::string, SessionState::ToolParamValue>& vals,
                     AnslScriptEngine& eng)
{
    if (vals.empty() || !eng.HasParams())
        return;

    auto enum_value_valid = [](const AnslParamSpec& spec, const std::string& v) -> bool {
        if (spec.type != AnslParamType::Enum)
            return true;
        for (const std::string& item : spec.enum_items)
            if (item == v)
                return true;
        return false;
    };

    for (const auto& spec : eng.GetParamSpecs())
    {
        if (spec.key.empty())
            continue;
        const auto it = vals.find(spec.key);
        if (it == vals.end())
            continue;
        const SessionState::ToolParamValue& v = it->second;
        if (v.type != (int)spec.type)
            continue;

        switch (spec.type)
        {
            case AnslParamType::Bool:  (void)eng.SetParamBool(spec.key, v.b); break;
            case AnslParamType::Int:   (void)eng.SetParamInt(spec.key, v.i); break;
            case AnslParamType::Float: (void)eng.SetParamFloat(spec.key, v.f); break;
            case AnslParamType::Enum:
            {
                if (enum_value_valid(spec, v.s))
                    (void)eng.SetParamEnum(spec.key, v.s);
            } break;
            case AnslParamType::Button:
                break;
        }
    }
}

bool LoadToolParamPresetsFromFile(const char* path,
                                  std::vector<ToolParamPreset>& out_presets,
                                  std::unordered_map<std::string, int>& out_selected_slot,
                                  std::string& error)
{
    error.clear();
    out_presets.clear();
    out_selected_slot.clear();
    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }

    std::ifstream f(path);
    if (!f)
    {
        error = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!j.is_object())
    {
        error = "Expected JSON object in tool-presets.json";
        return false;
    }

    const int schema_version =
        (j.contains("schema_version") && j["schema_version"].is_number_integer())
            ? j["schema_version"].get<int>()
            : 1;

    // Schema v2:
    // - selected = { "<tool_id>": <slot_int 1..9>, ... }
    // - presets[] = { tool_id, slot, title, params{...} }
    //
    // Schema v1 (legacy):
    // - selected = { "<tool_id>": "<title>", ... }
    // - presets[] = { tool_id, title, params{...} }
    //
    // We migrate v1 to v2 semantics on load by assigning slots 1..9 in file order per tool.
    if (j.contains("selected") && j["selected"].is_object())
    {
        for (auto it = j["selected"].begin(); it != j["selected"].end(); ++it)
        {
            const std::string tool_id = it.key();
            if (tool_id.empty())
                continue;

            if (schema_version >= 2)
            {
                if (!it.value().is_number_integer())
                    continue;
                const int slot = it.value().get<int>();
                if (slot >= 1 && slot <= 9)
                    out_selected_slot[tool_id] = slot;
            }
            else
            {
                // v1 selected-by-title migration is handled after presets are loaded.
                // We temporarily ignore it here.
            }
        }
    }

    if (!j.contains("presets") || !j["presets"].is_array())
    {
        // Allow empty/partial files: treat as no presets, but not a hard failure.
        return true;
    }

    // Legacy v1 selected-by-title map (tool_id -> title)
    std::unordered_map<std::string, std::string> v1_selected_title;
    if (schema_version < 2 && j.contains("selected") && j["selected"].is_object())
    {
        for (auto it = j["selected"].begin(); it != j["selected"].end(); ++it)
        {
            if (!it.value().is_string())
                continue;
            const std::string tool_id = it.key();
            const std::string title = TrimCopy(it.value().get<std::string>());
            if (!tool_id.empty() && !title.empty())
                v1_selected_title[tool_id] = title;
        }
    }

    // v1 slot assignment per tool (file order).
    std::unordered_map<std::string, int> v1_next_slot;

    for (const auto& item : j["presets"])
    {
        if (!item.is_object())
            continue;
        ToolParamPreset p;

        if (schema_version >= 2)
        {
            if (item.contains("slot") && item["slot"].is_number_integer())
                p.slot = item["slot"].get<int>();
        }
        if (item.contains("title") && item["title"].is_string())
            p.title = TrimCopy(item["title"].get<std::string>());
        if (item.contains("tool_id") && item["tool_id"].is_string())
            p.tool_id = item["tool_id"].get<std::string>();
        if (p.title.empty() || p.tool_id.empty())
            continue;

        if (schema_version < 2)
        {
            int& next = v1_next_slot[p.tool_id];
            if (next <= 0)
                next = 1;
            p.slot = next;
            next = std::min(10, next + 1); // clamp; slot validity checked below
        }

        if (p.slot < 1 || p.slot > 9)
            continue;

        if (!item.contains("params") || !item["params"].is_object())
            continue;

        for (auto it = item["params"].begin(); it != item["params"].end(); ++it)
        {
            const std::string key = it.key();
            const json& vj = it.value();
            if (key.empty() || !vj.is_object())
                continue;

            if (!vj.contains("type") || !vj["type"].is_number_integer())
                continue;

            SessionState::ToolParamValue v;
            v.type = vj["type"].get<int>();
            if (vj.contains("b") && vj["b"].is_boolean()) v.b = vj["b"].get<bool>();
            if (vj.contains("i") && vj["i"].is_number_integer()) v.i = vj["i"].get<int>();
            if (vj.contains("f") && vj["f"].is_number()) v.f = vj["f"].get<float>();
            if (vj.contains("s") && vj["s"].is_string()) v.s = vj["s"].get<std::string>();

            p.values[key] = std::move(v);
        }

        if (!p.values.empty())
            out_presets.push_back(std::move(p));
    }

    // Ensure at most one preset per (tool_id, slot). Last one wins.
    {
        std::unordered_map<std::string, size_t> key_to_idx;
        key_to_idx.reserve(out_presets.size());
        std::vector<ToolParamPreset> dedup;
        dedup.reserve(out_presets.size());
        for (auto& p : out_presets)
        {
            const std::string k = p.tool_id + "#" + std::to_string(p.slot);
            auto it = key_to_idx.find(k);
            if (it == key_to_idx.end())
            {
                key_to_idx[k] = dedup.size();
                dedup.push_back(std::move(p));
            }
            else
            {
                dedup[it->second] = std::move(p);
            }
        }
        out_presets = std::move(dedup);
    }

    // Finish v1 migration: map selected-by-title -> selected slot.
    if (schema_version < 2 && !v1_selected_title.empty())
    {
        for (const auto& kv : v1_selected_title)
        {
            const std::string& tool_id = kv.first;
            const std::string& want_title = kv.second;
            for (const auto& p : out_presets)
            {
                if (p.tool_id == tool_id && p.title == want_title)
                {
                    out_selected_slot[tool_id] = p.slot;
                    break;
                }
            }
        }
    }

    return true;
}

bool SaveToolParamPresetsToFile(const char* path,
                                const std::vector<ToolParamPreset>& presets,
                                const std::unordered_map<std::string, int>& selected_slot,
                                std::string& error)
{
    error.clear();
    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }

    json j;
    j["schema_version"] = 2;

    json sel = json::object();
    for (const auto& kv : selected_slot)
        if (!kv.first.empty() && kv.second >= 1 && kv.second <= 9)
            sel[kv.first] = kv.second;
    j["selected"] = std::move(sel);

    json arr = json::array();
    for (const auto& p : presets)
    {
        if (p.title.empty() || p.tool_id.empty() || p.values.empty())
            continue;
        if (p.slot < 1 || p.slot > 9)
            continue;
        json item;
        item["slot"] = p.slot;
        item["title"] = p.title;
        item["tool_id"] = p.tool_id;
        json params = json::object();
        for (const auto& pv : p.values)
        {
            if (pv.first.empty())
                continue;
            const SessionState::ToolParamValue& v = pv.second;
            json vj;
            vj["type"] = v.type;
            if (v.type == 0 || v.type == 4) vj["b"] = v.b;
            else if (v.type == 1) vj["i"] = v.i;
            else if (v.type == 2) vj["f"] = v.f;
            else if (v.type == 3) vj["s"] = v.s;
            params[pv.first] = std::move(vj);
        }
        item["params"] = std::move(params);
        arr.push_back(std::move(item));
    }
    j["presets"] = std::move(arr);

    std::ofstream f(path);
    if (!f)
    {
        error = std::string("Failed to write ") + path;
        return false;
    }
    try
    {
        f << j.dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    return true;
}
} // namespace tool_params


