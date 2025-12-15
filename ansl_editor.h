#pragma once

#include "imgui.h"
#include "layer_manager.h"
#include "ansl_script_engine.h"

#include <string>
#include <vector>

// A simple IMGUI component: a Play/Pause toggle button + a multiline text editor
// that expands to fill the remaining available space.
class AnslEditor
{
public:
    AnslEditor();

    bool IsPlaying() const { return playing_; }
    void SetPlaying(bool playing) { playing_ = playing; }
    void TogglePlaying() { playing_ = !playing_; }

    std::string&       Text() { return text_; }
    const std::string& Text() const { return text_; }
    void SetText(std::string text)
    {
        text_ = std::move(text);
        // Keep behavior consistent whether edits come from typing or programmatic loads:
        // the next UI tick should recompile and re-apply script settings (fps/once/background).
        needs_recompile_ = true;
    }

    // Render the component. `id` must be unique within the current ImGui window.
    // `flags` are passed through to ImGui::InputTextMultiline.
    void Render(const char* id,
                const std::vector<LayerManagerCanvasRef>& canvases,
                AnslScriptEngine& engine,
                int current_fg_xterm = -1,
                int current_bg_xterm = -1,
                ImGuiInputTextFlags flags = 0);

private:
    struct ExampleSpec
    {
        std::string path;   // full path to .lua example file
        std::string label;  // filename (or friendly label)
    };

    // Loads examples from `examples_dir_` into `examples_` (non-recursive).
    // Returns false on error; `error` will be populated.
    bool LoadExamplesFromDirectory(std::string& error);

    bool        playing_ = false;
    std::string text_;

    // Target selection
    int target_canvas_id_ = 0;
    bool clear_layer_each_frame_ = true;

    // Example scripts dropdown
    std::string              examples_dir_ = "assets/ansl-examples";
    std::vector<ExampleSpec> examples_;
    bool                     examples_loaded_ = false;
    std::string              examples_error_;
    int                      selected_example_index_ = -1; // -1 = none

    // Playback / throttling
    int    target_fps_ = 30;
    double last_tick_time_ = 0.0;
    double accumulator_ = 0.0;
    double measured_script_fps_ = 0.0;
    double fps_window_start_ = 0.0;
    int    fps_window_frames_ = 0;
    int    script_frame_ = 0;
    bool   pending_run_once_ = false;
    // When a script is in `settings.once` mode, pressing Play should:
    // - show "Pause" briefly (next UI frame)
    // - run exactly one tick
    // - stop (returning to "Play")
    // To achieve the brief "Pause" state, we defer the one-shot execution by one UI frame.
    bool   pending_once_play_deferred_ = false;
    bool   script_once_ = false;
    bool   script_once_ran_ = false;

    // Engine state
    bool        needs_recompile_ = true;
    std::string last_error_;
};
