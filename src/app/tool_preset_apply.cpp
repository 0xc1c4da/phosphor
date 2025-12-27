#include "app/tool_preset_apply.h"

#include <unordered_map>
#include <vector>

#include "core/paths.h"

#include "io/session/session_state.h"
#include "ui/tool_params.h"

namespace app
{
bool ApplyToolPresetDigit(const std::string& tool_id,
                          int digit,
                          AnslScriptEngine& tool_engine,
                          SessionState& session)
{
    if (tool_id.empty())
        return false;
    if (digit < 1 || digit > 9)
        return false;

    const std::string presets_path = PhosphorAssetPath("tool-presets.json");
    std::vector<tool_params::ToolParamPreset> presets;
    std::unordered_map<std::string, int> selected_by_tool_slot;
    std::string err;
    if (!tool_params::LoadToolParamPresetsFromFile(presets_path.c_str(), presets, selected_by_tool_slot, err))
        return false;

    const tool_params::ToolParamPreset* found = nullptr;
    for (const auto& p : presets)
    {
        if (p.tool_id == tool_id && p.slot == digit)
        {
            found = &p;
            break;
        }
    }
    if (!found)
        return false;

    selected_by_tool_slot[tool_id] = digit;
    tool_params::ApplyToolParams(found->values, tool_engine);
    tool_params::SaveToolParamsToSession(session, tool_id, tool_engine);
    if (!tool_params::SaveToolParamPresetsToFile(presets_path.c_str(), presets, selected_by_tool_slot, err))
        return false;

    return true;
}
} // namespace app


