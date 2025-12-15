#pragma once

#include <string>
#include <vector>

// Tool Palette:
// - scans assets/tools/*.lua
// - reads global `settings = { icon = "...", label = "..." }`
// - exposes a selected active tool (by file path)

struct ToolSpec
{
    std::string path;   // full path to .lua tool file
    std::string icon;   // UTF-8 glyph shown on the button
    std::string label;  // human-friendly name
};

class ToolPalette
{
public:
    // Loads tool specs from a directory (non-recursive).
    bool LoadFromDirectory(const std::string& tools_dir, std::string& error);

    // Renders the palette as an ImGui window. Returns true if the active tool changed.
    bool Render(const char* title, bool* p_open);

    int GetActiveToolIndex() const { return active_index_; }
    const ToolSpec* GetActiveTool() const;
    const std::vector<ToolSpec>& GetTools() const { return tools_; }

    // If the active tool changed since the last call, returns true and writes the active tool path.
    bool TakeActiveToolChanged(std::string& out_path);

    // If the user pressed Refresh, returns true and clears the flag.
    bool TakeReloadRequested();
    const std::string& GetToolsDir() const { return tools_dir_; }

private:
    std::vector<ToolSpec> tools_;
    int  active_index_ = 0;
    bool active_changed_ = false;
    bool reload_requested_ = false;
    std::string tools_dir_;

    static bool ParseToolSettingsFromLuaFile(const std::string& path, ToolSpec& out, std::string& error);
};


