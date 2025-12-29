#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ansl/ansl_script_engine.h"
#include "io/session/session_state.h"
#include "ui/tool_palette.h"
#include "ui/tool_params.h"

namespace app { class FocusRouter; }

class ToolPresetsWindow
{
public:
    ToolPresetsWindow();

    // Keep the window's in-memory selection in sync with external actions (e.g. Ctrl+1..9).
    // This is purely UI state; persistence is handled elsewhere.
    void NotifySelectedSlot(const std::string& tool_id, int slot);

    // Returns true if window was shown (built).
    bool Render(const ToolSpec* active_tool,
                const std::string& tool_id,
                AnslScriptEngine& tool_engine,
                SessionState& session,
                bool* p_open,
                bool apply_placement_this_frame,
                app::FocusRouter* focus_router = nullptr);

private:
    void EnsureLoaded();
    bool Load();
    bool Save();

    // Editable preset file path.
    std::string path_;

    bool loaded_ = false;
    bool dirty_ = false;
    std::string last_error_;

    std::vector<tool_params::ToolParamPreset> presets_;
    // tool_id -> selected slot (1..9)
    std::unordered_map<std::string, int> selected_slot_by_tool_;

    // Rename modal state
    bool rename_modal_open_ = false;
    int rename_global_index_ = -1;
    std::string rename_new_title_;
};


