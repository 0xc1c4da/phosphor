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

    std::string query_;
    int selected_index_ = 0;

    // Optional debug/telemetry state (not persisted).
    bool show_debug_window_ = false;
    int  dbg_open_count_ = 0;
    int  dbg_close_count_ = 0;
    double dbg_opened_at_s_ = 0.0;
    bool   dbg_recorded_first_result_ = false;
    double dbg_time_to_first_result_ms_ = 0.0;
    int    dbg_last_result_count_ = 0;
    std::string dbg_last_selected_id_;
    std::string dbg_last_selected_kind_;
    std::string dbg_last_executed_id_;
    std::string dbg_last_executed_kind_;
    bool dbg_last_execute_success_ = false;

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
    int active_colour_lane_ = 0; // 0=FG, 1=BG
    int fg_lane_index_ = 0;
    int bg_lane_index_ = 0;
    std::vector<Item> fg_lane_;
    std::vector<Item> bg_lane_;

    void rebuild_all_items(const RenderContext& ctx);
    void rebuild_results(const RenderContext& ctx);
};


