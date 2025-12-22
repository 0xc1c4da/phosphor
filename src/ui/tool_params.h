#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ansl/ansl_script_engine.h"
#include "io/session/session_state.h"

namespace tool_params
{
// ---------------------------------------------------------------------
// Tool parameter persistence + presets (assets/tool-presets.json)
// ---------------------------------------------------------------------

struct ToolParamPreset
{
    std::string title;
    std::string tool_id;
    std::unordered_map<std::string, SessionState::ToolParamValue> values;
};

// Persist current tool params -> session.tool_param_values[tool_id]
void SaveToolParamsToSession(SessionState& session, const std::string& tool_id, const AnslScriptEngine& eng);

// Restore saved params from session into the engine (best-effort, validates enum values).
void RestoreToolParamsFromSession(const SessionState& session, const std::string& tool_id, AnslScriptEngine& eng);

// Capture/Apply param values for presets.
bool CaptureToolParams(const AnslScriptEngine& eng,
                       std::unordered_map<std::string, SessionState::ToolParamValue>& out);
void ApplyToolParams(const std::unordered_map<std::string, SessionState::ToolParamValue>& vals,
                     AnslScriptEngine& eng);

// File-backed presets + selected preset per tool.
bool LoadToolParamPresetsFromFile(const char* path,
                                  std::vector<ToolParamPreset>& out_presets,
                                  std::unordered_map<std::string, std::string>& out_selected,
                                  std::string& error);
bool SaveToolParamPresetsToFile(const char* path,
                                const std::vector<ToolParamPreset>& presets,
                                const std::unordered_map<std::string, std::string>& selected,
                                std::string& error);
} // namespace tool_params


