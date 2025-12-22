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
// - Provides file-backed presets UI via title-bar ⋮ popup
class ToolParametersWindow
{
public:
    ToolParametersWindow();

    // Returns true if the window was shown (i.e. engine has params and window was built).
    bool Render(const ToolSpec* active_tool,
                const std::string& compiled_tool_id,
                AnslScriptEngine& tool_engine,
                SessionState& session,
                bool apply_placement_this_frame);

private:
    void EnsurePresetsLoaded();
    void HandlePresetFileOps();

    void RenderPresetsPopup(const char* base_id,
                            const std::string& tool_id,
                            AnslScriptEngine& tool_engine,
                            SessionState& session,
                            ImGuiWindowFlags flags);

private:
    // Presets persistence state (formerly static locals in run_frame.cpp).
    bool presets_loaded_ = false;
    std::string presets_path_;
    std::string presets_error_;
    bool request_reload_ = false;
    bool request_save_ = false;
    std::vector<tool_params::ToolParamPreset> presets_;
    std::unordered_map<std::string, std::string> selected_by_tool_; // tool_id -> preset title

    // Modal state
    bool open_new_popup_ = false;
    bool open_rename_popup_ = false;
    bool open_delete_popup_ = false;
    char new_title_buf_[256] = {};
    char rename_title_buf_[256] = {};
};


