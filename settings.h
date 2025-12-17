#pragma once

#include <functional>
#include <string>
#include <vector>

// Forward declarations to avoid pulling imgui headers into all compilation units.
struct ImGuiTextFilter;

// Settings window with an extendable tab system.
// For now it hosts the Key Bindings editor (load/edit/save JSON in assets/key-bindings.json).
class SettingsWindow
{
public:
    struct KeyBinding
    {
        bool        enabled  = true;
        std::string chord;      // e.g. "Ctrl+Shift+Z", "Alt+B", "Left"
        std::string context;    // e.g. "global", "editor", "selection"
        std::string platform;   // "any", "windows", "linux", "macos"
    };

    struct Action
    {
        std::string            id;          // internal stable id, e.g. "app.file.new"
        std::string            title;       // UI label
        std::string            category;    // grouping (File/Edit/View/Selection/...)
        std::string            description; // optional help text
        std::vector<KeyBinding> bindings;
    };

    struct Tab
    {
        std::string id;    // stable internal id
        std::string title; // visible label
        std::function<void()> render;
    };

    SettingsWindow();

    void SetOpen(bool open) { open_ = open; }
    bool IsOpen() const { return open_; }

    // Extendable: allows future subsystems to register additional tabs/panels.
    // If a tab with the same id exists, it is replaced.
    void RegisterTab(const Tab& tab);

    // Loads/Saves the keybindings JSON from disk.
    bool LoadKeyBindingsFromFile(const std::string& path, std::string& out_error);
    bool SaveKeyBindingsToFile(const std::string& path, std::string& out_error) const;

    // Main render call. Safe to call every frame; does nothing if closed.
    void Render(const char* title = "Settings");

private:
    void EnsureDefaultTabsRegistered();
    void RenderTab_KeyBindings();

    static std::vector<Action> DefaultActions(); // used if no JSON exists / parse fails

private:
    bool open_ = false;

    // Tabs
    bool                  tabs_registered_ = false;
    std::vector<Tab>      tabs_;
    std::string           active_tab_id_;

    // Keybindings model
    std::string           keybindings_path_ = "assets/key-bindings.json";
    std::vector<Action>   actions_;
    bool                  loaded_ = false;
    bool                  dirty_ = false;
    std::string           last_error_;

    // UI state
    std::string           filter_text_;
    bool                  show_ids_ = false;

    // "Record binding" capture state (UI-only for now).
    bool                  capture_active_ = false;
    size_t                capture_action_idx_ = 0;
    size_t                capture_binding_idx_ = 0;
};


