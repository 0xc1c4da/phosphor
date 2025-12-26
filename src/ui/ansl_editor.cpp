#include "ui/ansl_editor.h"

#include "imgui.h"
#include "core/paths.h"
#include "core/canvas.h"
#include "core/i18n.h"
#include "core/xterm256_palette.h"
#include "ui/ansl_params_ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace fs = std::filesystem;

static bool PaletteRefEqual(const phos::colour::PaletteRef& a, const phos::colour::PaletteRef& b)
{
    return a.is_builtin == b.is_builtin &&
           a.builtin == b.builtin &&
           a.uid == b.uid;
}

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
        error = PHOS_TRF("ansl_editor.examples_dir_not_found_fmt", phos::i18n::Arg::Str(examples_dir_));
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

    examples_dir_ = PhosphorAssetPath("ansl-examples");

    // Helpful starter template.
    if (text_.empty())
    {
        text_ =
            "-- Define a global render(ctx, layer) function.\\n"
            "-- ctx = { cols, rows, frame, time, fg, bg, metrics={aspect=...}, cursor={x,y,pressed,p={...}} }\\n"
            "-- Modules are available as `ansl.*` (num, sdf, vec2, vec3, colour, buffer, drawbox, string).\\n"
            "-- Tip: you can also do `local ansl = require('ansl')` if you prefer not to use globals.\\n"
            "-- layer supports:\\n"
            "--   layer:set(x, y, cpOrString, fg?, bg?)   -- fg/bg are indices in the active canvas palette (or nil)\\n"
            "--   layer:get(x, y) -> ch, fg, bg           -- fg/bg are indices in the active canvas palette (or nil when unset)\\n"
            "--   layer:clear(cpOrString?)\\n"
            "--   layer:setRow(y, utf8String)\\n"
            "\\n"
            "-- Colours are indices in the active canvas palette (no alpha). Helpers:\\n"
            "--   ansl.colour.rgb(r,g,b) -> idx\\n"
            "--   ansl.colour.hex('#RRGGBB') -> idx\\n"
            "--   ansl.colour.ansi16.bright_white, etc (ANSI16/VGA16 names mapped into the active palette)\\n"
            "-- ctx.fg / ctx.bg expose the editor's current FG/BG selection when available.\\n"
            "\\n"
            "function render(ctx, layer)\\n"
            "  -- Example: moving dot\\n"
            "  local x = (ctx.frame %% ctx.cols)\\n"
            "  local y = math.floor((ctx.frame / 2) %% ctx.rows)\\n"
            "  local fg = ctx.fg or ansl.colour.ansi16.bright_white\\n"
            "  local bg = ctx.bg -- nil means unset\\n"
            "  layer:set(x, y, '@', fg, bg)\\n"
            "end\\n";
    }
}

void AnslEditor::SetTargetFps(int fps)
{
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    target_fps_ = fps;
}

std::string AnslEditor::SelectedExampleLabel() const
{
    if (selected_example_index_ < 0 || (size_t)selected_example_index_ >= examples_.size())
        return {};
    return examples_[(size_t)selected_example_index_].label;
}

std::string AnslEditor::SelectedExamplePath() const
{
    if (selected_example_index_ < 0 || (size_t)selected_example_index_ >= examples_.size())
        return {};
    return examples_[(size_t)selected_example_index_].path;
}

void AnslEditor::SetSelectedExamplePreference(int index, std::string label, std::string path)
{
    preferred_example_index_ = index;
    preferred_example_label_ = std::move(label);
    preferred_example_path_ = std::move(path);
    has_example_preference_ = true;
}

void AnslEditor::Render(const char* id,
                        AnsiCanvas* active_canvas,
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
    const std::string play_label = playing_ ? PHOS_TR("ansl_editor.pause") : PHOS_TR("ansl_editor.play");
    bool request_play = false;
    bool request_pause = false;
    bool request_run_once = false;
    if (ImGui::Button((play_label + "##ansl_play_pause").c_str()))
    {
        if (playing_)
            request_pause = true;
        else
            request_play = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted((playing_ ? PHOS_TR("ansl_editor.playing") : PHOS_TR("ansl_editor.paused")).c_str());
    if (script_once_)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted((script_once_ran_ ? PHOS_TR("ansl_editor.once_ran") : PHOS_TR("ansl_editor.once")).c_str());
    }

    ImGui::Separator();

    if (!active_canvas)
    {
        ImGui::TextUnformatted(PHOS_TR("ansl_editor.open_canvas_to_run").c_str());
    }
    else
    {
        AnsiCanvas* canvas = active_canvas;
        // Always target the canvas's active ("current") layer.
        const int active_layer = canvas ? canvas->GetActiveLayerIndex() : 0;
        ImGui::TextUnformatted(PHOS_TRF("ansl_editor.target_layer_active_fmt",
                                        phos::i18n::Arg::I64((long long)active_layer)).c_str());

        ImGui::Checkbox(PHOS_TR("ansl_editor.clear_layer_each_frame").c_str(), &clear_layer_each_frame_);

        // FPS control + measured script FPS.
        if (target_fps_ < 1) target_fps_ = 1;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string fps_lbl = PHOS_TR("ansl_editor.script_fps") + "###ansl_script_fps";
        ImGui::SliderInt(fps_lbl.c_str(), &target_fps_, 1, 240);
        ImGui::TextUnformatted(PHOS_TRF("ansl_editor.measured_script_fps_fmt",
                                        phos::i18n::Arg::F64(measured_script_fps_)).c_str());

        // Compile/run controls.
        bool compile_clicked = ImGui::Button((PHOS_TR("ansl_editor.compile") + "###ansl_compile").c_str());
        ImGui::SameLine();
        bool run_once_clicked = ImGui::Button((PHOS_TR("ansl_editor.run_once") + "###ansl_run_once").c_str());

        // Examples dropdown.
        ImGui::Separator();
        if (ImGui::SmallButton((PHOS_TR("ansl_editor.refresh_examples") + "###ansl_refresh_examples").c_str()))
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
                examples_error_ = err.empty()
                    ? PHOS_TRF("ansl_editor.no_examples_found_in_fmt", phos::i18n::Arg::Str(examples_dir_))
                    : err;
            else
                examples_error_.clear();

            // Apply restored selection preference, if any.
            if (has_example_preference_ && !examples_.empty())
            {
                auto find_index = [&](const std::string& path,
                                      const std::string& label) -> std::optional<int>
                {
                    if (!path.empty())
                    {
                        for (size_t i = 0; i < examples_.size(); ++i)
                            if (examples_[i].path == path)
                                return (int)i;
                    }
                    if (!label.empty())
                    {
                        for (size_t i = 0; i < examples_.size(); ++i)
                            if (examples_[i].label == label)
                                return (int)i;
                    }
                    return std::nullopt;
                };

                if (auto idx = find_index(preferred_example_path_, preferred_example_label_))
                {
                    selected_example_index_ = *idx;
                }
                else if (preferred_example_index_ >= -1 &&
                         preferred_example_index_ < (int)examples_.size())
                {
                    selected_example_index_ = preferred_example_index_;
                }

                has_example_preference_ = false;
                preferred_example_index_ = -1;
                preferred_example_label_.clear();
                preferred_example_path_.clear();
            }

            // Keep selection stable if possible; otherwise reset.
            if (selected_example_index_ >= (int)examples_.size())
                selected_example_index_ = -1;

            examples_loaded_ = true;
        }

        if (!examples_.empty())
        {
            std::vector<const char*> labels;
            labels.reserve(examples_.size() + 1);
            const std::string none = PHOS_TR("ansl_editor.example_none");
            labels.push_back(none.c_str());
            for (const auto& ex : examples_)
                labels.push_back(ex.label.c_str());

            int combo_index = selected_example_index_ + 1; // -1 -> 0 ("<none>")
            ImGui::SetNextItemWidth(-FLT_MIN);
            const std::string example_lbl = PHOS_TR("ansl_editor.example") + "###ansl_example";
            if (ImGui::Combo(example_lbl.c_str(), &combo_index, labels.data(), (int)labels.size()))
            {
                selected_example_index_ = combo_index - 1;
                if (selected_example_index_ >= 0 && selected_example_index_ < (int)examples_.size())
                {
                    const std::string src = ReadFileToString(examples_[(size_t)selected_example_index_].path);
                    if (src.empty())
                    {
                        last_error_ = PHOS_TRF("ansl_editor.failed_to_read_example_fmt",
                                               phos::i18n::Arg::Str(examples_[(size_t)selected_example_index_].path));
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
                // Treat script-driven fills as tool/script mutations so they respect selection clipping.
                AnsiCanvas::ToolRunScope scope(*c);
                std::optional<AnsiCanvas::Colour32> fg;
                std::optional<AnsiCanvas::Colour32> bg;
                if (s.has_foreground)
                    fg = (AnsiCanvas::Colour32)xterm256::Colour32ForIndex(s.foreground_xterm);
                if (s.has_background)
                    bg = (AnsiCanvas::Colour32)xterm256::Colour32ForIndex(s.background_xterm);
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

            // If the active canvas palette has changed since the last successful compile,
            // force a recompile so palette-dependent constants (ansl.colour.*) are re-quantized
            // into the new palette index space.
            if (canvas && engine.HasRenderFunction())
            {
                if (!has_compiled_palette_ref_)
                {
                    needs_recompile_ = true;
                }
                else if (!PaletteRefEqual(compiled_palette_ref_, canvas->GetPaletteRef()))
                {
                    needs_recompile_ = true;
                }
            }

            if (!needs_recompile_)
                return engine.HasRenderFunction();

            std::string err;
            if (!engine.CompileUserScript(text_, canvas, err))
            {
                last_error_ = err;
                playing_ = false;
                return false;
            }

            last_error_.clear();
            needs_recompile_ = false;
            if (canvas)
            {
                compiled_palette_ref_ = canvas->GetPaletteRef();
                has_compiled_palette_ref_ = true;
            }
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
            // Palette conversion can happen while paused/playing. Ensure we compile against
            // the current palette before executing any tick.
            if (!EnsureCompiled(/*for_execution=*/true))
                should_run = false;
        }

        if (canvas && should_run)
        {
            // Ensure ANSL scripts behave like tools: respect selection-as-mask and mirror-mode,
            // while keeping core operations (I/O, undo replay) unaffected.
            AnsiCanvas::ToolRunScope scope(*canvas);
            // Performance: scripts frequently touch many cells per tick. When running scripts
            // outside AnsiCanvas::Render(), we are not capturing undo deltas, so we can batch
            // state/content revision bumps to once per script tick.
            AnsiCanvas::ExternalMutationScope batch(*canvas);

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
            int cx = 0, cy = 0, half_y = 0, pcx = 0, pcy = 0, phalf_y = 0;
            bool left_down = false, right_down = false;
            bool prev_left_down = false, prev_right_down = false;
            if (canvas->GetCursorCell(cx, cy, half_y, left_down, right_down, pcx, pcy, phalf_y, prev_left_down, prev_right_down))
            {
                fctx.cursor_x = cx;
                fctx.cursor_y = cy;
                fctx.cursor_half_y = half_y;
                fctx.cursor_left_down = left_down;
                fctx.cursor_right_down = right_down;
                fctx.cursor_px = pcx;
                fctx.cursor_py = pcy;
                fctx.cursor_phalf_y = phalf_y;
                fctx.cursor_prev_left_down = prev_left_down;
                fctx.cursor_prev_right_down = prev_right_down;
            }

            std::string err;
            const int layer_index = canvas->GetActiveLayerIndex();
            ToolCommandSink cmds;
            cmds.allow_tool_commands = false;
            cmds.out_commands = nullptr;
            if (!engine.RunFrame(*canvas, layer_index, fctx, cmds, clear_layer_each_frame_, err))
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
