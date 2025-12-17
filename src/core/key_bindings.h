#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kb
{
// Key binding schema + runtime evaluation engine.
//
// - Stores actions (id/title/category/description) each with 1+ bindings.
// - Loads/saves `assets/key-bindings.json` (schema_version=1).
// - Evaluates chord presses via Dear ImGui key state (IsKeyPressed + modifier state).
// - Supports platform + context gating.

enum class Platform : std::uint8_t
{
    Any = 0,
    Windows,
    Linux,
    MacOS,
};

enum class Context : std::uint8_t
{
    Global = 0,
    Editor,
    Selection,
    Canvas,
};

struct KeyBinding
{
    bool        enabled  = true;
    std::string chord;    // e.g. "Ctrl+Shift+Z", "Alt+B", "Left", "F1"
    std::string context;  // "global", "editor", "selection", "canvas"
    std::string platform; // "any", "windows", "linux", "macos"
};

struct Action
{
    std::string             id;          // stable internal id, e.g. "app.file.new"
    std::string             title;       // UI label
    std::string             category;    // grouping (File/Edit/View/Selection/...)
    std::string             description; // optional help text
    std::vector<KeyBinding> bindings;
};

struct Mods
{
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
    bool super = false;
};

struct ParsedChord
{
    Mods    mods;
    ImGuiKey key = ImGuiKey_None;
    bool     any_enter = false; // if true, match Enter OR KeypadEnter
};

// Parses a chord string like "Ctrl+Shift+Z" into a normalized chord.
// Returns false on parse error (err contains human-readable message).
bool ParseChordString(const std::string& chord, ParsedChord& out, std::string& err);

// Runtime platform (compile-time best effort).
Platform RuntimePlatform();

// Runtime evaluation context (what is currently "active").
struct EvalContext
{
    bool     global = true;
    bool     editor = false;
    bool     selection = false;
    bool     canvas = false;
    Platform platform = Platform::Any;
};

struct Hotkeys
{
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool select_all = false;
    bool cancel = false;
    bool delete_selection = false;
};

class KeyBindingsEngine
{
public:
    KeyBindingsEngine();

    void SetDefaults(std::vector<Action> defaults);
    void SetToolActions(std::vector<Action> tool_actions); // optional (registered by tools)

    bool LoadFromFile(const std::string& path, std::string& out_error);
    bool SaveToFile(const std::string& path, std::string& out_error) const;

    // Access for UI editing.
    const std::vector<Action>& Actions() const { return actions_; }
    std::vector<Action>& ActionsMutable();

    const std::string& Path() const { return path_; }
    void SetPath(std::string path) { path_ = std::move(path); }

    bool IsLoaded() const { return loaded_; }
    bool IsDirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; runtime_dirty_ = true; }
    void ClearDirty() { dirty_ = false; }
    const std::string& LastError() const { return last_error_; }

    // Evaluates whether a given action's chord was pressed this frame.
    // This only checks key state + chord matching; the caller should gate based on
    // focus/popups as appropriate for their UI flow.
    bool ActionPressed(std::string_view action_id, const EvalContext& ctx) const;

    // Convenience: common editing hotkeys used by selection tools.
    Hotkeys EvalCommonHotkeys(const EvalContext& ctx) const;

private:
    struct RuntimeBinding
    {
        bool        enabled = true;
        Context     ctx = Context::Global;
        Platform    platform = Platform::Any;
        ParsedChord chord;
        std::string chord_text; // for debugging/errors (optional)
    };

    struct RuntimeAction
    {
        std::string id;
        std::vector<RuntimeBinding> bindings;
    };

    static std::vector<Action> MergeDefaultsWithFile(const std::vector<Action>& defaults_plus_tools,
                                                     const std::vector<Action>& file_actions);
    void RebuildRuntime() const;

private:
    std::string path_ = "assets/key-bindings.json";
    bool        loaded_ = false;
    bool        dirty_ = false;
    std::string last_error_;

    std::vector<Action> defaults_;
    std::vector<Action> tool_actions_;

    std::vector<Action> actions_;

    // Cached runtime representation (parsed chords + fast lookup).
    mutable bool runtime_dirty_ = true;
    mutable std::unordered_map<std::string, size_t> action_index_by_id_;
    mutable std::vector<RuntimeAction> runtime_actions_;
};

// Built-in default actions (seeded from references/hotkeys.md, plus editor-specific defaults).
std::vector<Action> DefaultActions();

} // namespace kb


