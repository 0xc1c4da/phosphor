#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ansl/ansl_script_engine.h"
#include "io/session/session_state.h"
#include "ui/tool_palette.h"
#include "ui/tool_params.h"

// Tool Parameters window:
// - Renders ANSL tool params UI (settings.params -> ctx.params)
namespace app { class FocusRouter; }
class ToolParametersWindow
{
public:
    ToolParametersWindow();

    // Returns true if the window was shown (window was built).
    bool Render(const ToolSpec* active_tool,
                const std::string& compiled_tool_id,
                AnslScriptEngine& tool_engine,
                SessionState& session,
                bool apply_placement_this_frame,
                app::FocusRouter* focus_router = nullptr);
};


