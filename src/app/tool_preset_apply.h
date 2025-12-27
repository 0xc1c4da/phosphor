#pragma once

#include <string>

struct SessionState;
class AnslScriptEngine;

namespace app
{
// Apply preset slot (1..9) for the given tool id.
// Returns true if the preset exists and was applied + persisted.
bool ApplyToolPresetDigit(const std::string& tool_id,
                          int digit,
                          AnslScriptEngine& tool_engine,
                          SessionState& session);
} // namespace app


