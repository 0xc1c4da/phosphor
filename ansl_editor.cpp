#include "ansl_editor.h"

#include "imgui.h"
#include "canvas.h"
#include "xterm256_palette.h"
#include "ansl_params_ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace fs = std::filesystem;

static std::string ReadFileToString(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// ImGui helper to edit std::string via InputText* with automatic resize.
// Adapted from Dear ImGui's imgui_demo.cpp.
static int InputTextCallback_Resize(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* str = static_cast<std::string*>(data->UserData);
        IM_ASSERT(str != nullptr);
        IM_ASSERT(data->Buf == str->c_str());

        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = const_cast<char*>(str->c_str());
    }
    return 0;
}

static bool InputTextMultilineString(const char* label,
                                     std::string* str,
                                     const ImVec2& size,
                                     ImGuiInputTextFlags flags)
{
    IM_ASSERT(str != nullptr);

    // Ensure there is always a NUL-terminated buffer large enough for current contents.
    const size_t min_cap = std::max<size_t>(1024u, str->size() + 1u);
    if (str->capacity() < min_cap)
        str->reserve(min_cap);

    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label,
                                     const_cast<char*>(str->c_str()),
                                     str->capacity() + 1u,
                                     size,
                                     flags,
                                     InputTextCallback_Resize,
                                     str);
}

bool AnslEditor::LoadExamplesFromDirectory(std::string& error)
{
    examples_.clear();

    fs::path dir(examples_dir_);
    if (!fs::exists(dir) || !fs::is_directory(dir))
    {
        error = "Examples dir not found: " + examples_dir_;
        return false;
    }

    std::vector<ExampleSpec> found;
    try
    {
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            fs::path p = entry.path();
            if (p.extension() != ".lua")
                continue;

            ExampleSpec ex;
            ex.path = p.string();
            ex.label = p.filename().string();
            found.push_back(std::move(ex));
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    std::sort(found.begin(), found.end(), [](const ExampleSpec& a, const ExampleSpec& b) {
        if (a.label != b.label) return a.label < b.label;
        return a.path < b.path;
    });

    examples_ = std::move(found);
    error.clear();
    return !examples_.empty();
}

AnslEditor::AnslEditor()
{
    // Provide a tiny bit of initial capacity so typing doesn't immediately resize every frame.
    text_.reserve(1024);

    // Helpful starter template.
    if (text_.empty())
    {
        text_ =
            "-- Define a global render(ctx, layer) function.\\n"
            "-- ctx = { cols, rows, frame, time, fg, bg, metrics={aspect=...}, cursor={x,y,pressed,p={...}} }\\n"
            "-- Modules are available as `ansl.*` (num, sdf, vec2, vec3, color, buffer, drawbox, string).\\n"
            "-- Tip: you can also do `local ansl = require('ansl')` if you prefer not to use globals.\\n"
            "-- layer supports:\\n"
            "--   layer:set(x, y, cpOrString, fg?, bg?)   -- fg/bg are xterm-256 indices (0..255) or nil\\n"
            "--   layer:get(x, y) -> ch, fg, bg           -- fg/bg are xterm-256 indices or nil when unset\\n"
            "--   layer:clear(cpOrString?)\\n"
            "--   layer:setRow(y, utf8String)\\n"
            "\\n"
            "-- Colors are xterm-256 indices (no alpha). Helpers:\\n"
            "--   ansl.color.rgb(r,g,b) -> idx\\n"
            "--   ansl.color.hex('#RRGGBB') -> idx\\n"
            "--   ansl.color.ansi16.red -> 1, etc\\n"
            "-- ctx.fg / ctx.bg expose the editor's current FG/BG selection when available.\\n"
            "\\n"
            "function render(ctx, layer)\\n"
            "  -- Example: moving dot\\n"
            "  local x = (ctx.frame %% ctx.cols)\\n"
            "  local y = math.floor((ctx.frame / 2) %% ctx.rows)\\n"
            "  local fg = ctx.fg or ansl.color.ansi16.bright_white\\n"
            "  local bg = ctx.bg -- nil means unset\\n"
            "  layer:set(x, y, '@', fg, bg)\\n"
            "end\\n";
    }
}

void AnslEditor::Render(const char* id,
                        const std::vector<LayerManagerCanvasRef>& canvases,
                        AnslScriptEngine& engine,
                        int current_fg_xterm,
                        int current_bg_xterm,
                        ImGuiInputTextFlags flags)
{
    if (!id)
        id = "ansl_editor";

    ImGui::PushID(id);

    // Top row: playback.
    // Always expose a stable Play/Pause button label.
    // (Changing this label to "Run Once" caused an ImGui ID collision with the dedicated
    // "Run Once" button below when scripts use `settings.once = true`.)
    const char* play_label = playing_ ? "Pause" : "Play";
    bool request_play = false;
    bool request_pause = false;
    bool request_run_once = false;
    if (ImGui::Button(play_label))
    {
        if (playing_)
            request_pause = true;
        else
            request_play = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted(playing_ ? "Playing" : "Paused");
    if (script_once_)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(script_once_ran_ ? "(once: ran)" : "(once)");
    }

    ImGui::Separator();

    if (canvases.empty())
    {
        ImGui::TextUnformatted("Open a Canvas window to run scripts.");
    }
    else
    {
        // Target canvas selection.
        if (target_canvas_id_ == 0)
            target_canvas_id_ = canvases.front().id;

        std::vector<std::string> canvas_strings;
        std::vector<const char*> canvas_labels;
        canvas_strings.reserve(canvases.size());
        canvas_labels.reserve(canvases.size());
        for (const auto& c : canvases)
            canvas_strings.push_back("Canvas " + std::to_string(c.id));
        for (const std::string& s : canvas_strings)
            canvas_labels.push_back(s.c_str());

        int canvas_index = 0;
        for (size_t i = 0; i < canvases.size(); ++i)
        {
            if (canvases[i].id == target_canvas_id_)
            {
                canvas_index = (int)i;
                break;
            }
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("Target Canvas", &canvas_index, canvas_labels.data(), (int)canvas_labels.size()))
            target_canvas_id_ = canvases[(size_t)canvas_index].id;

        AnsiCanvas* canvas = canvases[(size_t)canvas_index].canvas;
        // Always target the canvas's active ("current") layer.
        const int active_layer = canvas ? canvas->GetActiveLayerIndex() : 0;
        ImGui::Text("Target Layer: %d (active)", active_layer);

        ImGui::Checkbox("Clear layer each frame", &clear_layer_each_frame_);

        // FPS control + measured script FPS.
        if (target_fps_ < 1) target_fps_ = 1;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("Script FPS", &target_fps_, 1, 240);
        ImGui::Text("Measured script FPS: %.1f", measured_script_fps_);

        // Compile/run controls.
        bool compile_clicked = ImGui::Button("Compile");
        ImGui::SameLine();
        bool run_once_clicked = ImGui::Button("Run Once");

        // Examples dropdown.
        ImGui::Separator();
        if (ImGui::SmallButton("Refresh Examples"))
        {
            examples_loaded_ = false;
            examples_error_.clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", examples_dir_.c_str());

        if (!examples_loaded_)
        {
            std::string err;
            if (!LoadExamplesFromDirectory(err))
                examples_error_ = err.empty() ? ("No examples found in " + examples_dir_) : err;
            else
                examples_error_.clear();

            // Keep selection stable if possible; otherwise reset.
            if (selected_example_index_ >= (int)examples_.size())
                selected_example_index_ = -1;

            examples_loaded_ = true;
        }

        if (!examples_.empty())
        {
            std::vector<const char*> labels;
            labels.reserve(examples_.size() + 1);
            labels.push_back("<none>");
            for (const auto& ex : examples_)
                labels.push_back(ex.label.c_str());

            int combo_index = selected_example_index_ + 1; // -1 -> 0 ("<none>")
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("Example", &combo_index, labels.data(), (int)labels.size()))
            {
                selected_example_index_ = combo_index - 1;
                if (selected_example_index_ >= 0 && selected_example_index_ < (int)examples_.size())
                {
                    const std::string src = ReadFileToString(examples_[(size_t)selected_example_index_].path);
                    if (src.empty())
                    {
                        last_error_ = "Failed to read example: " + examples_[(size_t)selected_example_index_].path;
                    }
                    else
                    {
                        // Overwrite editor text and stop playback (script content changed).
                        SetText(src);
                        last_error_.clear();
                        playing_ = false;
                        pending_run_once_ = false;
                        pending_once_play_deferred_ = false;
                        script_once_ran_ = false;
                    }
                }
            }
        }
        else if (!examples_error_.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", examples_error_.c_str());
        }

        // If the user starts playback or triggers Run Once, create a single undo snapshot
        // before the script begins mutating the canvas. We intentionally do NOT track
        // undo steps for every frame while playing.
        bool pushed_execution_snapshot = false;
        auto PushExecutionUndoSnapshot = [&]()
        {
            if (pushed_execution_snapshot)
                return;
            if (canvas)
            {
                canvas->PushUndoSnapshot();
                pushed_execution_snapshot = true;
            }
        };

        // ---- Compilation + settings application (single source of truth) ----
        auto ResetPlaybackState = [&]()
        {
            script_frame_ = 0;
            script_once_ran_ = false;
            pending_run_once_ = false;
            pending_once_play_deferred_ = false;
            last_tick_time_ = 0.0;
            accumulator_ = 0.0;
            fps_window_start_ = 0.0;
            fps_window_frames_ = 0;
            measured_script_fps_ = 0.0;
        };

        auto ApplyScriptSettings = [&](AnsiCanvas* c)
        {
            const AnslScriptSettings s = engine.GetSettings();
            script_once_ = s.once;
            if (s.has_fps)
                target_fps_ = s.fps;
            if (script_once_)
                playing_ = false;

            // One-shot fg/bg fill (also re-applied per-frame on clear in the engine).
            if (c && (s.has_foreground || s.has_background))
            {
                std::optional<AnsiCanvas::Color32> fg;
                std::optional<AnsiCanvas::Color32> bg;
                if (s.has_foreground)
                    fg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(s.foreground_xterm);
                if (s.has_background)
                    bg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(s.background_xterm);
                const int layer_index = c->GetActiveLayerIndex();
                c->FillLayer(layer_index, std::nullopt, fg, bg);
            }
        };

        auto EnsureCompiled = [&](bool for_execution) -> bool
        {
            if (compile_clicked)
                needs_recompile_ = true;

            // If we are about to execute and nothing has been compiled yet, force a compile.
            if (for_execution && !engine.HasRenderFunction())
                needs_recompile_ = true;

            if (!needs_recompile_)
                return engine.HasRenderFunction();

            std::string err;
            if (!engine.CompileUserScript(text_, err))
            {
                last_error_ = err;
                playing_ = false;
                return false;
            }

            last_error_.clear();
            needs_recompile_ = false;
            ResetPlaybackState();
            ApplyScriptSettings(canvas);
            return true;
        };

        // Decide whether to run this frame (Run Once bypasses the limiter).
        bool should_run = false;

        // Normalize execution requests from both top-row and Run Once button.
        if (run_once_clicked)
            request_run_once = true;

        // If any request could trigger execution, compile first and apply settings (fps/once/background).
        const bool wants_execution = request_play || request_run_once || (compile_clicked && script_once_);
        if (wants_execution)
        {
            if (!EnsureCompiled(/*for_execution=*/true))
            {
                // Compile failed; don't attempt to run.
                request_play = request_pause = request_run_once = false;
            }
        }
        else
        {
            // Still honor explicit Compile even if it won't execute.
            if (compile_clicked)
                (void)EnsureCompiled(/*for_execution=*/false);
        }

        // If we deferred a once-mode "Play" from the previous UI frame, arm the actual one-shot run now.
        // This makes the button show "Pause" for one frame before executing and returning to "Play".
        if (script_once_ && pending_once_play_deferred_)
        {
            pending_run_once_ = true;
            pending_once_play_deferred_ = false;
        }

        // Apply requested state transitions *after* compilation/settings so fps/once are current.
        if (request_pause)
        {
            playing_ = false;
            pending_run_once_ = false;
            pending_once_play_deferred_ = false;
        }
        else if (request_play)
        {
            if (script_once_)
            {
                // In once mode, "Play" means: briefly enter Playing, then run one tick on the next UI frame.
                PushExecutionUndoSnapshot();
                playing_ = true;
                last_tick_time_ = 0.0; // re-sync timing
                pending_once_play_deferred_ = true;
            }
            else
            {
                // Starting playback: snapshot the pre-script state.
                PushExecutionUndoSnapshot();
                playing_ = true;
                last_tick_time_ = 0.0; // re-sync timing on resume
            }
        }

        if (request_run_once)
        {
            // One-shot execution: snapshot the pre-script state.
            PushExecutionUndoSnapshot();
            playing_ = false;
            script_frame_ = 0;
            script_once_ran_ = false;
            pending_run_once_ = true;
            pending_once_play_deferred_ = false;
        }

        // Compile button behavior for once scripts: compile + run one frame.
        if (compile_clicked && script_once_ && !script_once_ran_)
        {
            PushExecutionUndoSnapshot();
            pending_run_once_ = true;
        }

        // Once scripts stop after the first executed tick.
        if (script_once_ && script_once_ran_)
            playing_ = false;

        if (pending_run_once_)
        {
            should_run = true;
            pending_run_once_ = false;
        }
        else if (playing_)
        {
            const double now = ImGui::GetTime();
            if (last_tick_time_ <= 0.0)
            {
                last_tick_time_ = now;
                accumulator_ = 0.0;
                fps_window_start_ = now;
                fps_window_frames_ = 0;
            }

            const double dt = now - last_tick_time_;
            last_tick_time_ = now;
            if (dt > 0.0)
                accumulator_ += dt;

            const double interval = 1.0 / (double)std::max(1, target_fps_);
            if (accumulator_ >= interval)
            {
                // Run at most one script tick per UI frame; drop excess time.
                accumulator_ = std::fmod(accumulator_, interval);
                should_run = true;
            }

            // Update measured script FPS over a rolling window (~1s).
            const double window_dt = now - fps_window_start_;
            if (window_dt >= 1.0)
            {
                measured_script_fps_ = (window_dt > 0.0) ? ((double)fps_window_frames_ / window_dt) : 0.0;
                fps_window_start_ = now;
                fps_window_frames_ = 0;
            }
        }

        if (canvas && should_run)
        {
            AnslFrameContext fctx;
            fctx.cols = canvas->GetColumns();
            fctx.rows = canvas->GetRows();
            // Keep ANSL-style time/frame consistent under throttling.
            // - `frame` increments only when the script runs.
            // - `time` is milliseconds (classic ANSL runner convention).
            fctx.frame = script_frame_;
            fctx.time = ImGui::GetTime() * 1000.0;
            fctx.metrics_aspect = canvas->GetLastCellAspect();
            fctx.fg = current_fg_xterm;
            fctx.bg = current_bg_xterm;

            // Caret position comes from the canvas caret (keyboard/editing).
            canvas->GetCaretCell(fctx.caret_x, fctx.caret_y);

            // Cursor/button state comes from the canvas mouse cursor (cell-space).
            int cx = 0, cy = 0, pcx = 0, pcy = 0;
            bool left_down = false, right_down = false;
            bool prev_left_down = false, prev_right_down = false;
            if (canvas->GetCursorCell(cx, cy, left_down, right_down, pcx, pcy, prev_left_down, prev_right_down))
            {
                fctx.cursor_x = cx;
                fctx.cursor_y = cy;
                fctx.cursor_left_down = left_down;
                fctx.cursor_right_down = right_down;
                fctx.cursor_px = pcx;
                fctx.cursor_py = pcy;
                fctx.cursor_prev_left_down = prev_left_down;
                fctx.cursor_prev_right_down = prev_right_down;
            }

            std::string err;
            const int layer_index = canvas->GetActiveLayerIndex();
            if (!engine.RunFrame(*canvas, layer_index, fctx, clear_layer_each_frame_, err))
                last_error_ = err;

            // Count only executed script frames.
            fps_window_frames_++;
            script_frame_++;
            if (script_once_)
                script_once_ran_ = true;
        }

        // Script parameters UI (settings.params -> ctx.params)
        if (engine.HasParams())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
                (void)RenderAnslParamsUI("script_params", engine);
        }

        if (!last_error_.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());
        }
    }

    // Multiline editor filling remaining space.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f) avail.x = 1.0f;
    if (avail.y < 1.0f) avail.y = 1.0f;

    // A hidden label so it doesn't consume layout width; ID uniqueness comes from PushID().
    if (InputTextMultilineString("##text", &text_, avail, flags))
        needs_recompile_ = true;

    ImGui::PopID();
}
