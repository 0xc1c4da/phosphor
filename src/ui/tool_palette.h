#pragma once

#include <string>
#include <vector>

#include "core/key_bindings.h"

struct SessionState;

// Tool Palette:
// - scans assets/tools/*.lua
// - reads global `settings = { icon = "...", label = "..." }`
// - exposes a selected active tool (by file path)

struct ToolSpec
{
    // Stable identifier used for tool history + script-driven activation.
    // Sourced from Lua `settings.id` (preferred). If omitted, it may be derived by the host.
    std::string id;
    std::string path;   // full path to .lua tool file
    std::string icon;   // UTF-8 glyph shown on the button
    std::string label;  // human-friendly name

    // Optional tool-registered actions (for keybinding engine + Settings UI).
    // Tools may define these under `settings.actions` in their Lua file.
    std::vector<kb::Action> actions;

    // Optional action routing hints (used by host Action Router).
    //
    // Tools can declare which actions they handle, and whether they should be considered
    // when the tool is active (highest precedence) or inactive (fallback tier).
    //
    // Lua schema:
    //   settings.handles = {
    //     { action = "selection.delete", when = "active" },
    //     { action = "selection.delete", when = "inactive" },
    //   }
    //
    // Back-compat: older tools may still use:
    //   settings.claims = { "id", ... }          -> when="active"
    //   settings.fallbackClaims = { "id", ... }  -> when="inactive"
    enum class HandleWhen : std::uint8_t
    {
        Active = 0,
        Inactive,
    };
    struct HandleRule
    {
        std::string action;
        HandleWhen  when = HandleWhen::Active;
    };
    std::vector<HandleRule> handles;
};

class ToolPalette
{
public:
    // Loads tool specs from a directory (non-recursive).
    bool LoadFromDirectory(const std::string& tools_dir, std::string& error);

    // Renders the palette as an ImGui window. Returns true if the active tool changed.
    bool Render(const char* title, bool* p_open, SessionState* session = nullptr, bool apply_placement_this_frame = false);

    int GetActiveToolIndex() const { return active_index_; }
    const ToolSpec* GetActiveTool() const;
    const std::vector<ToolSpec>& GetTools() const { return tools_; }

    // If the active tool changed since the last call, returns true and writes the active tool path.
    bool TakeActiveToolChanged(std::string& out_path);

    // If the user pressed Refresh, returns true and clears the flag.
    bool TakeReloadRequested();
    const std::string& GetToolsDir() const { return tools_dir_; }

    // Restore selection from a previously saved tool path.
    // Returns true if a matching tool was found and selection changed.
    bool SetActiveToolByPath(const std::string& path);

    // Activate by stable id (ToolSpec::id).
    // Returns true if a matching tool was found and selection changed.
    bool SetActiveToolById(const std::string& id);

private:
    std::vector<ToolSpec> tools_;
    int  active_index_ = 0;
    bool active_changed_ = false;
    bool reload_requested_ = false;
    std::string tools_dir_;

    static bool ParseToolSettingsFromLuaFile(const std::string& path, ToolSpec& out, std::string& error);
};


