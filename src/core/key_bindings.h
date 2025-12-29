#pragma once

#include "imgui.h"

#include <cstddef>
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
    // If true, treat this binding as "repeat while held" using ImGui's key-repeat timing
    // (ImGuiIO::KeyRepeatDelay / ImGuiIO::KeyRepeatRate).
    //
    // Important for actions like Undo/Redo: holding Ctrl+Z should repeatedly undo after a short delay.
    bool repeat = false;
    // Internal: distinguishes "repeat was specified in JSON" vs inherited/defaulted.
    // This allows older key-bindings.json files (without a repeat field) to inherit new defaults.
    bool repeat_set = false;
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

    // Collect a list of action ids whose bindings are pressed this frame for the given EvalContext.
    //
    // - Returned string_views reference stable internal storage (valid until bindings are rebuilt).
    // - Callers must treat them as frame-local and not persist them.
    // - The scan is deterministic (runtime action order) and de-duplicates actions with multiple bindings.
    void CollectPressedActions(const EvalContext& ctx,
                               std::vector<std::string_view>& out,
                               size_t max_actions = 64) const;

    // Convenience: common editing hotkeys used by selection tools.
    Hotkeys EvalCommonHotkeys(const EvalContext& ctx) const;

    // UI helper: returns a human-readable summary of enabled chord(s) for an action
    // on the given runtime platform, joined with " / " (e.g. "Ctrl+S / Cmd+S").
    // Returns empty string if no enabled chords exist.
    std::string ChordTextSummaryForAction(std::string_view action_id,
                                          Platform runtime_platform,
                                          size_t max_chords = 3) const;

    // UI helper: returns the "best" single chord for an action, preferring:
    // - an enabled binding matching `preferred_context`
    // - then an enabled binding in "global"
    // - then any enabled binding
    // filtered by `runtime_platform`.
    //
    // Returns empty string if no enabled chord exists.
    std::string BestChordForAction(std::string_view action_id,
                                   std::string_view preferred_context,
                                   Platform runtime_platform) const;

private:
    struct RuntimeBinding
    {
        bool        enabled = true;
        Context     ctx = Context::Global;
        Platform    platform = Platform::Any;
        ParsedChord chord;
        bool        repeat = false;
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

// UI helper for callers that only have an action list (e.g. fall back to DefaultActions()).
// Filters enabled bindings by runtime platform and returns a joined string like "Ctrl+Z / Cmd+Z".
// Returns empty string if no enabled chords exist.
std::string ChordTextSummaryForAction(const std::vector<Action>& actions,
                                      std::string_view action_id,
                                      Platform runtime_platform,
                                      size_t max_chords = 3);

// UI helper: returns the "best" single chord for an action on the given platform.
// See KeyBindingsEngine::BestChordForAction() for selection rules.
std::string BestChordForAction(const std::vector<Action>& actions,
                               std::string_view action_id,
                               std::string_view preferred_context,
                               Platform runtime_platform);

// Built-in default actions (seeded from references/hotkeys.md, plus editor-specific defaults).
std::vector<Action> DefaultActions();

} // namespace kb


