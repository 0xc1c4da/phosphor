#pragma once

#include "ansl/ansl_script_engine.h"

// Shared IMGUI widget for rendering host-managed ANSL parameters (settings.params -> ctx.params).
//
// Usage:
//   if (ImGui::CollapsingHeader("Parameters")) { RenderAnslParamsUI("params", engine); }
//
// Returns true if any parameter value was changed this frame.
bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine);


