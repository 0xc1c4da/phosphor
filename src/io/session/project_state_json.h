#pragma once

#include "core/canvas.h"

#include <nlohmann/json.hpp>

#include <string>

// Shared conversion helpers for AnsiCanvas::ProjectState <-> nlohmann::json.
// Used by both the .phos project IO layer and session restore.
namespace project_state_json
{
using json = nlohmann::json;

json ToJson(const AnsiCanvas::ProjectState& st);
bool FromJson(const json& j, AnsiCanvas::ProjectState& out, std::string& err);
} // namespace project_state_json


