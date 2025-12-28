#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ImVec4;

namespace kb { class KeyBindingsEngine; }

struct SessionState;
class ToolPalette;
class AnsiCanvas;
class AnslScriptEngine;

namespace app { struct ActionExecContext; }

class CommandPalette
{
public:
    enum class Mode
    {
        Default = 0,
        Colour = 1,
    };

    struct WindowToggle
    {
        std::string key;          // stable id for MRU
        std::string label;        // display
        std::string imgui_focus;  // window name/id for ImGui::SetWindowFocus (may be empty)
        bool* toggle = nullptr;   // persisted UI toggle
    };

    struct RenderContext
    {
        kb::KeyBindingsEngine& keybinds;
        SessionState& session;
        ToolPalette& tool_palette;

        AnsiCanvas* active_canvas = nullptr;
        ImVec4& fg_colour;
        ImVec4& bg_colour;
        // Optional: shared "active foreground/background focus" used by the colour picker.
        // Semantics: 0 = foreground, 1 = background.
        // When provided, the command palette will:
        // - target '#...' (without fg:/bg:) to the active focus
        // - update it when the user interacts with FG/BG lanes (keeps semantics consistent across UI)
        int* active_fb = nullptr;

        const app::ActionExecContext& action_exec;
        AnslScriptEngine& tool_engine; // active tool engine (compiled tool script)
        // Tool id that is currently compiled into the active tool engine (best effort).
        // Used for tool-claimed action routing parity with keybindings.
        std::string_view compiled_tool_id;
        std::function<void(std::string_view tool_id)> activate_tool_by_id;
        // Apply preset slot (1..9) for the currently active/compiled tool.
        // Should return true if a preset was found and applied.
        std::function<bool(int digit)> apply_active_tool_preset_digit;
        std::function<void()> request_focus_last_canvas;
        // Optional: request that an ImGui window be focused after the palette closes.
        // Intended for "Focus <panel>" items, since those panels may be created later in the frame.
        std::function<void(std::string_view imgui_window_name)> request_focus_imgui_window;

        std::vector<WindowToggle> windows;
    };

    void Open(Mode mode = Mode::Default);
    void Close();
    bool IsOpen() const { return is_open_; }

    void Render(const RenderContext& ctx);

private:
    Mode mode_ = Mode::Default;
    bool is_open_ = false;
    bool open_requested_ = false;
    bool focus_query_on_open_ = false;
    bool colour_lane_interacted_ = false; // true when user navigates/clicks colour lanes (enables Enter-to-apply)

    std::string query_;
    int selected_index_ = 0;

    struct Item
    {
        enum class Kind
        {
            Action,
            Tool,
            ToolPreset,
            Window,
            ColourFg,
            ColourBg,
        };
        Kind kind = Kind::Action;
        std::string id;
        std::string label;
        std::string detail;
        bool disabled = false;
        std::string disabled_reason;

        // Optional payloads (kind-dependent).
        int preset_digit = 0; // 1..9
        std::uint32_t rgba32 = 0; // ImGui ABGR packed (0 means unset/invalid)
    };

    std::vector<Item> all_items_;
    std::vector<Item> results_;

    // Colour-lane UI state (spec: two lanes with independent selection).
    int fg_lane_index_ = 0;
    int bg_lane_index_ = 0;
    std::vector<Item> fg_lane_;
    std::vector<Item> bg_lane_;

    // Dominant-colour cache (to avoid re-histogramming every time the query changes).
    struct DominantCacheEntry
    {
        // Store as int to avoid requiring full `AnsiCanvas` definition in this header.
        // This corresponds to a palette index in the active canvas (see `AnsiCanvas::ColourIndex16`).
        int idx = -1;
        std::uint32_t rgba32 = 0;
    };
    struct DominantCache
    {
        const AnsiCanvas* canvas = nullptr;
        std::uint64_t content_rev = 0;
        int c0 = 0, c1 = 0, r0 = 0, r1 = 0;
        int cols = 0, rows = 0;
        std::vector<DominantCacheEntry> fg;
        std::vector<DominantCacheEntry> bg;
    };
    DominantCache dominant_cache_;

    void rebuild_all_items(const RenderContext& ctx);
    void rebuild_results(const RenderContext& ctx);
};


