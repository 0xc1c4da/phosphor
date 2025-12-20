#include "app/run_frame.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include "app/app_state.h"
#include "app/app_ui.h"
#include "app/workspace.h"
#include "app/workspace_persist.h"
#include "app/vulkan_state.h"
#include "app/canvas_preview_texture.h"

#include "core/paths.h"
#include "core/xterm256_palette.h"

#include "ansl/ansl_native.h"
#include "ansl/ansl_script_engine.h"

#include "io/file_dialog_tags.h"
#include "io/image_loader.h"
#include "io/io_manager.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/open_canvas_cache.h"
#include "io/session/session_state.h"
#include "io/session/imgui_persistence.h"

#include "ui/ansl_editor.h"
#include "ui/ansl_params_ui.h"
#include "ui/character_palette.h"
#include "ui/character_picker.h"
#include "ui/character_set.h"
#include "ui/colour_picker.h"
#include "ui/colour_palette.h"
#include "ui/export_dialog.h"
#include "ui/glyph_token.h"
#include "ui/image_to_chafa_dialog.h"
#include "ui/image_window.h"
#include "ui/imgui_window_chrome.h"
#include "ui/layer_manager.h"
#include "ui/minimap_window.h"
#include "ui/settings.h"
#include "ui/sixteen_colors_browser.h"
#include "ui/tool_palette.h"

namespace app
{

void RunFrame(AppState& st)
{
    if (st.interrupt_requested && st.interrupt_requested())
    {
        st.done = true;
        return;
    }

    // Alias state to keep the code close to the former `main.cpp` implementation.
    SDL_Window* window = st.platform.window;
    VulkanState& vk = *st.vulkan.vk;
    ImGui_ImplVulkanH_Window* wd = st.vulkan.wd;
    SessionState& session_state = *st.persist.session_state;
    kb::KeyBindingsEngine& keybinds = *st.services.keybinds;
    IoManager& io_manager = *st.services.io_manager;
    SdlFileDialogQueue& file_dialogs = *st.services.file_dialogs;
    ExportDialog& export_dialog = *st.services.export_dialog;
    SettingsWindow& settings_window = *st.services.settings_window;
    ToolPalette& tool_palette = *st.tools.tool_palette;
    AnslEditor& ansl_editor = *st.tools.ansl_editor;
    AnslScriptEngine& ansl_engine = *st.tools.ansl_engine;
    AnslScriptEngine& tool_engine = *st.tools.tool_engine;
    CharacterPicker& character_picker = *st.ui.character_picker;
    CharacterPalette& character_palette = *st.ui.character_palette;
    CharacterSetWindow& character_sets = *st.ui.character_sets;
    LayerManager& layer_manager = *st.ui.layer_manager;
    ImageToChafaDialog& image_to_chafa_dialog = *st.ui.image_to_chafa_dialog;
    MinimapWindow& minimap_window = *st.ui.minimap_window;
    CanvasPreviewTexture& preview_texture = *st.ui.preview_texture;
    SixteenColorsBrowserWindow& sixteen_browser = *st.ui.sixteen_browser;

    auto& canvases = *st.workspace.canvases;
    auto& images = *st.workspace.images;
    int& next_canvas_id = *st.workspace.next_canvas_id;
    int& next_image_id = *st.workspace.next_image_id;
    int& last_active_canvas_id = *st.workspace.last_active_canvas_id;

    bool& show_demo_window = *st.toggles.show_demo_window;
    ImVec4& clear_color = *st.colors.clear_color;
    bool& show_color_picker_window = *st.toggles.show_color_picker_window;
    bool& show_character_picker_window = *st.toggles.show_character_picker_window;
    bool& show_character_palette_window = *st.toggles.show_character_palette_window;
    bool& show_character_sets_window = *st.toggles.show_character_sets_window;
    bool& show_layer_manager_window = *st.toggles.show_layer_manager_window;
    bool& show_ansl_editor_window = *st.toggles.show_ansl_editor_window;
    bool& show_tool_palette_window = *st.toggles.show_tool_palette_window;
    bool& show_minimap_window = *st.toggles.show_minimap_window;
    bool& show_settings_window = *st.toggles.show_settings_window;
    bool& show_16colors_browser_window = *st.toggles.show_16colors_browser_window;
    bool& window_fullscreen = *st.toggles.window_fullscreen;

    ImVec4& fg_color = *st.colors.fg_color;
    ImVec4& bg_color = *st.colors.bg_color;
    int& active_fb = *st.colors.active_fb;
    int& xterm_picker_mode = *st.colors.xterm_picker_mode;
    int& xterm_selected_palette = *st.colors.xterm_selected_palette;
    int& xterm_picker_preview_fb = *st.colors.xterm_picker_preview_fb;
    float& xterm_picker_last_hue = *st.colors.xterm_picker_last_hue;

    // ---------------------------------------------------------------------
    // Colour palettes (loaded from assets/color-palettes.json)
    // ---------------------------------------------------------------------
    // Tools (e.g. Smudge) need the active palette even if the Colour Picker window
    // isn't open, so we cache the palette defs here at the RunFrame scope.
    static bool                         palettes_loaded    = false;
    static std::vector<ColourPaletteDef> palettes;
    static std::string                  palettes_error;
    if (!palettes_loaded)
    {
        LoadColourPalettesFromJson(PhosphorAssetPath("color-palettes.json").c_str(), palettes, palettes_error);
        palettes_loaded = true;

        // Fallback if loading failed or file empty: single default HSV palette.
        if (!palettes_error.empty() || palettes.empty())
        {
            ColourPaletteDef def;
            def.title = "Default HSV";
            for (int n = 0; n < 32; ++n)
            {
                ImVec4 c;
                float h = n / 31.0f;
                ImGui::ColorConvertHSVtoRGB(h, 0.8f, 0.8f, c.x, c.y, c.z);
                c.w = 1.0f;
                def.colors.push_back(c);
            }
            palettes.clear();
            palettes.push_back(std::move(def));
            palettes_error.clear();
            xterm_selected_palette = 0;
        }
    }

    if (!palettes.empty())
    {
        if (xterm_selected_palette < 0 || xterm_selected_palette >= (int)palettes.size())
            xterm_selected_palette = 0;
    }

    std::uint32_t& tool_brush_cp = *st.tools.tool_brush_cp;
    std::string& tool_brush_utf8 = *st.tools.tool_brush_utf8;

    std::string& tools_error = *st.tools.tools_error;
    std::string& tool_compile_error = *st.tools.tool_compile_error;

    // NOTE: Temporary/session-managed canvases (no explicit file path) are implicitly persisted
    // by session cache + session.json, so they should not participate in the Quit "Save All" gate.
    auto should_prompt_save_on_quit = [&](const CanvasWindow& cw) -> bool {
        if (!cw.open)
            return false;
        if (!cw.canvas.IsModifiedSinceLastSave())
            return false;
        // Empty file path means "not explicitly saved/opened" (session-only/temporary).
        if (!cw.canvas.HasFilePath())
            return false;
        return true;
    };

    auto any_dirty_canvas = [&]() -> bool {
        for (const auto& cptr : canvases)
        {
            if (!cptr)
                continue;
            const CanvasWindow& cw = *cptr;
            if (should_prompt_save_on_quit(cw))
                return true;
        }
        return false;
    };

    auto count_dirty_canvases = [&]() -> int {
        int n = 0;
        for (const auto& cptr : canvases)
        {
            if (!cptr)
                continue;
            const CanvasWindow& cw = *cptr;
            if (should_prompt_save_on_quit(cw))
                ++n;
        }
        return n;
    };

    auto active_tool_id = [&]() -> std::string {
        if (st.tools.active_tool_id) return st.tools.active_tool_id();
        return {};
    };
    auto activate_prev_tool = [&]() {
        if (st.tools.activate_prev_tool) st.tools.activate_prev_tool();
    };
    auto activate_tool_by_id = [&](const std::string& id) {
        if (st.tools.activate_tool_by_id) st.tools.activate_tool_by_id(id);
    };

    // Idle throttling helpers
    auto now_s = []() -> double { return (double)SDL_GetTicks() / 1000.0; };
    if (st.last_input_s <= 0.0)
        st.last_input_s = now_s();

    // Some platforms (e.g. Linux portals) may require pumping events for dialogs.
    SDL_PumpEvents();

    // Poll and handle events.
    //
    // IMPORTANT: Throttle idle frames by waiting briefly for events instead of spinning and
    // redrawing continuously. This reduces idle GPU usage substantially.
    const double t0 = now_s();
    const SDL_WindowFlags wf = SDL_GetWindowFlags(window);
    const bool is_minimized_flag = (wf & SDL_WINDOW_MINIMIZED) != 0;
    const bool is_focused = (wf & SDL_WINDOW_INPUT_FOCUS) != 0;
    // Some UI features require continuous redraw even without user input (e.g. ANSL playback).
    // If we block waiting for events, we cap the entire app's frame rate and tank those features.
    const bool wants_continuous_redraw =
        (show_ansl_editor_window && ansl_editor.IsPlaying());

    const double idle_for_s = t0 - st.last_input_s;
    // Heuristic timeouts:
    // - When interacting (mouse button down) or recently active: don't block.
    // - Otherwise: cap redraw to ~20fps when focused, ~10fps when unfocused.
    int wait_ms = 0;
    if (!is_minimized_flag && !st.mouse_down_prev && !wants_continuous_redraw)
    {
        if (idle_for_s > 0.25)
            wait_ms = is_focused ? 50 : 100;
    }

    auto process_event = [&](const SDL_Event& event)
    {
        // Treat these as "activity" to keep UI responsive.
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MOVED:
            st.last_input_s = now_s();
            break;
        default:
            break;
        }

        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT)
            st.done = true;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window))
            st.done = true;
    };

    SDL_Event event;
    if (wait_ms > 0)
    {
        if (SDL_WaitEventTimeout(&event, wait_ms))
            process_event(event);
    }
    while (SDL_PollEvent(&event))
        process_event(event);

    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
    {
        SDL_Delay(10);
        return;
    }


    // Resize swap chain?
    int fb_width, fb_height;
    SDL_GetWindowSize(window, &fb_width, &fb_height);
    if (fb_width > 0 && fb_height > 0 &&
        (vk.swapchain_rebuild ||
         wd->Width != fb_width ||
         wd->Height != fb_height))
    {
        vk.ResizeMainWindow(wd, fb_width, fb_height);
    }

    // Start the Dear ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    st.frame_counter++;

    // Quit confirmation: convert immediate quit requests into a modal if there are dirty canvases.
    if (st.done && any_dirty_canvas())
    {
        st.done = false;
        st.quit_modal_open = true;
        st.quit_waiting_on_save = false;
        st.quit_save_queue_ids.clear();
        st.quit_save_queue_index = 0;
    }

    // Determine which canvas should receive keyboard-only actions (Undo/Redo shortcuts).
    // "Focused" is tracked by each AnsiCanvas instance (grid focus).
    //
    // IMPORTANT: grid focus can remain true even after the user switches to a different canvas
    // window via docking/tabbing/window chrome. For UI (menus), we want the *active window's*
    // canvas to drive state (e.g. Mirror Mode checkbox), not a stale focused grid in another window.
    AnsiCanvas* focused_canvas = nullptr;
    CanvasWindow* focused_canvas_window = nullptr;
    for (auto& cptr : canvases)
    {
        if (!cptr)
            continue;
        CanvasWindow& c = *cptr;
        if (!c.open)
            continue;
        if (!c.canvas.HasFocus())
            continue;
        // Only treat this as the focused canvas if it's also the last active canvas window.
        // Otherwise the UI "active canvas" should fall back to last_active_canvas_id below.
        if (last_active_canvas_id != -1 && c.id != last_active_canvas_id)
            continue;

        if (c.open && c.canvas.HasFocus())
        {
            focused_canvas = &c.canvas;
            focused_canvas_window = &c;
            if (last_active_canvas_id == -1)
                last_active_canvas_id = c.id;
            break;
        }
    }
    // Active canvas for global actions (File menu, Edit menu items, future actions):
    // - prefer the focused grid canvas
    // - otherwise use the last active canvas window
    // - otherwise fall back to the first open canvas
    AnsiCanvas* active_canvas = focused_canvas;
    CanvasWindow* active_canvas_window = focused_canvas_window;
    if (!focused_canvas && last_active_canvas_id != -1)
    {
        for (auto& cptr : canvases)
        {
            if (!cptr)
                continue;
            CanvasWindow& c = *cptr;
            if (c.open && c.id == last_active_canvas_id)
            {
                active_canvas = &c.canvas;
                active_canvas_window = &c;
                break;
            }
        }
    }
    if (!active_canvas)
    {
        for (auto& cptr : canvases)
        {
            if (!cptr)
                continue;
            CanvasWindow& c = *cptr;
            if (c.open)
            {
                active_canvas = &c.canvas;
                active_canvas_window = &c;
                break;
            }
        }
    }

    // Apply the user's global undo limit preference to all open canvases.
    // Convention: 0 = unlimited.
    for (auto& cptr : canvases)
    {
        if (!cptr || !cptr->open)
            continue;
        if (cptr->canvas.GetUndoLimit() != session_state.undo_limit)
            cptr->canvas.SetUndoLimit(session_state.undo_limit);
    }

    auto try_restore_canvas_from_cache = [&](CanvasWindow& cw) {
        if (!cw.restore_pending || cw.restore_attempted || cw.restore_phos_cache_rel.empty())
            return;
        if (st.frame_counter <= 1)
            return; // keep first frame snappy

        cw.restore_attempted = true;
        cw.restore_error.clear();

        std::string rerr;
        if (!open_canvas_cache::LoadCanvasFromSessionCachePhos(cw.restore_phos_cache_rel, cw.canvas, rerr))
        {
            cw.restore_error = rerr.empty() ? "Failed to restore cached project." : rerr;
            return;
        }
        cw.restore_pending = false;
        cw.canvas.SetUndoLimit(session_state.undo_limit);
    };

    // Session restore (cached .phos projects):
    // Restore at most one pending canvas per frame, prioritizing the active canvas.
    if (st.frame_counter >= 2)
    {
        if (active_canvas_window)
            try_restore_canvas_from_cache(*active_canvas_window);

        for (auto& cwptr : canvases)
        {
            if (!cwptr)
                continue;
            CanvasWindow& cw = *cwptr;
            if (!cw.open)
                continue;
            if (cw.restore_pending && !cw.restore_attempted && !cw.restore_phos_cache_rel.empty())
            {
                try_restore_canvas_from_cache(cw);
                break;
            }
        }
    }

    // Main menu bar: File > New Canvas, Quit
    auto create_new_canvas = [&]()
    {
        auto canvas_window = std::make_unique<CanvasWindow>();
        canvas_window->open = true;
        canvas_window->id = next_canvas_id++;
        canvas_window->canvas.SetKeyBindingsEngine(&keybinds);
        canvas_window->canvas.SetUndoLimit(session_state.undo_limit);

        // Create a new blank canvas with a single base layer.
        canvas_window->canvas.SetColumns(80);
        canvas_window->canvas.EnsureRowsPublic(25);
        canvas_window->canvas.MarkSaved();

        last_active_canvas_id = canvas_window->id;
        canvases.push_back(std::move(canvas_window));
    };

    // Common IoManager callbacks (used by menu + dialog dispatch).
    IoManager::Callbacks io_cbs;
    io_cbs.create_canvas = [&](AnsiCanvas&& c)
    {
        auto canvas_window = std::make_unique<CanvasWindow>();
        canvas_window->open = true;
        canvas_window->id = next_canvas_id++;
        canvas_window->canvas = std::move(c);
        canvas_window->canvas.SetKeyBindingsEngine(&keybinds);
        canvas_window->canvas.SetUndoLimit(session_state.undo_limit);
        canvas_window->canvas.MarkSaved();
        last_active_canvas_id = canvas_window->id;
        canvases.push_back(std::move(canvas_window));
    };
    io_cbs.create_image = [&](IoManager::Callbacks::LoadedImage&& li)
    {
        ImageWindow img;
        img.id = next_image_id++;
        img.path = std::move(li.path);
        img.width = li.width;
        img.height = li.height;
        img.pixels = std::move(li.pixels);
        img.open = true;
        images.push_back(std::move(img));
    };

    appui::RenderMainMenuBar(window, keybinds, session_state, io_manager, file_dialogs, io_cbs,
                             export_dialog, settings_window,
                             active_canvas, st.done, window_fullscreen,
                             show_color_picker_window,
                             show_character_picker_window,
                             show_character_palette_window,
                             show_character_sets_window,
                             show_layer_manager_window,
                             show_ansl_editor_window,
                             show_tool_palette_window,
                             show_minimap_window,
                             show_settings_window,
                             show_16colors_browser_window,
                             create_new_canvas);

    // Canvas closes are applied later in the frame (after rendering/popups),
    // so we can turn close attempts into "Save changes?" confirmation flows.
    std::vector<int> close_canvas_ids;
    close_canvas_ids.reserve(8);

    auto push_recent = [&](const std::string& p) {
        if (p.empty())
            return;
        auto& v = session_state.recent_files;
        v.erase(std::remove(v.begin(), v.end(), p), v.end());
        v.insert(v.begin(), p);
        const size_t kMaxRecent = 20;
        if (v.size() > kMaxRecent)
            v.resize(kMaxRecent);
    };

    auto find_canvas_by_id = [&](int id) -> CanvasWindow* {
        if (id <= 0)
            return nullptr;
        for (auto& cptr : canvases)
        {
            if (!cptr)
                continue;
            if (cptr->id == id)
                return cptr.get();
        }
        return nullptr;
    };

    auto quit_save_next = [&]() {
        while (st.quit_save_queue_index < st.quit_save_queue_ids.size())
        {
            const int id = st.quit_save_queue_ids[st.quit_save_queue_index];
            CanvasWindow* cw = find_canvas_by_id(id);
            if (!cw || !should_prompt_save_on_quit(*cw))
            {
                ++st.quit_save_queue_index;
                continue;
            }

            // This may save immediately (if the canvas has a local file path) OR open a Save As dialog.
            io_manager.SaveProject(window, file_dialogs, &cw->canvas);
            st.quit_waiting_on_save = true;
            return;
        }

        // All done.
        st.quit_waiting_on_save = false;
        st.quit_modal_open = false;
        st.done = true;
    };

    // Quit confirmation modal.
    if (st.quit_modal_open && !ImGui::IsPopupOpen("Quit##confirm_quit", ImGuiPopupFlags_AnyPopupId))
        ImGui::OpenPopup("Quit##confirm_quit");
    if (st.quit_modal_open)
    {
        // Ensure consistent modal placement (center of the application viewport).
        if (ImGuiViewport* vp = ImGui::GetMainViewport())
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Quit##confirm_quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const int dirty_n = count_dirty_canvases();
        if (dirty_n <= 0)
        {
            ImGui::Text("Quit Phosphor?");
            ImGui::Separator();
            if (ImGui::Button("Quit"))
            {
                st.quit_modal_open = false;
                st.done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                st.quit_modal_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            ImGui::Text("You have %d canvas%s with unsaved changes.", dirty_n, dirty_n == 1 ? "" : "es");
            ImGui::Text("Do you want to save your changes before quitting?");
            ImGui::Separator();

            if (!st.quit_waiting_on_save && ImGui::Button("Save All"))
            {
                st.quit_save_queue_ids.clear();
                st.quit_save_queue_index = 0;
                for (const auto& cptr : canvases)
                {
                    if (!cptr)
                        continue;
                    const CanvasWindow& cw = *cptr;
                    if (should_prompt_save_on_quit(cw))
                        st.quit_save_queue_ids.push_back(cw.id);
                }
                st.quit_modal_open = false;
                quit_save_next();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (!st.quit_waiting_on_save && ImGui::Button("Don't Save"))
            {
                st.quit_modal_open = false;
                st.done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                st.quit_modal_open = false;
                st.quit_save_queue_ids.clear();
                st.quit_save_queue_index = 0;
                st.quit_waiting_on_save = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // Dispatch completed native file dialogs (projects, import/export, image import).
    {
        SdlFileDialogResult r;
        while (file_dialogs.Poll(r))
        {
            if (export_dialog.HandleDialogResult(r, io_manager, active_canvas))
                continue;
            io_manager.HandleDialogResult(r, active_canvas, io_cbs);
        }
    }

    // Apply Save-dialog outcomes (used by close-confirm flows).
    {
        IoManager::SaveEvent ev;
        while (io_manager.TakeLastSaveEvent(ev))
        {
            if (ev.kind == IoManager::SaveEventKind::Success && !ev.path.empty())
                push_recent(ev.path);

            // Quit "Save All" flow sequencing.
            if (st.quit_waiting_on_save)
            {
                const int expected_id =
                    (st.quit_save_queue_index < st.quit_save_queue_ids.size())
                        ? st.quit_save_queue_ids[st.quit_save_queue_index]
                        : -1;
                CanvasWindow* expected = find_canvas_by_id(expected_id);
                if (expected && ev.canvas == &expected->canvas)
                {
                    st.quit_waiting_on_save = false;
                    if (ev.kind == IoManager::SaveEventKind::Success)
                    {
                        ++st.quit_save_queue_index;
                        quit_save_next();
                    }
                    else
                    {
                        // Failed/canceled: abort quit and return to the modal.
                        st.quit_modal_open = true;
                        st.quit_save_queue_ids.clear();
                        st.quit_save_queue_index = 0;
                    }
                }
            }

            if (!ev.canvas)
                continue;
            for (auto& cwptr : canvases)
            {
                if (!cwptr)
                    continue;
                CanvasWindow& cw = *cwptr;
                if (&cw.canvas != ev.canvas)
                    continue;
                if (cw.close_waiting_on_save)
                {
                    cw.close_waiting_on_save = false;
                    if (ev.kind == IoManager::SaveEventKind::Success)
                    {
                        cw.open = false;
                        close_canvas_ids.push_back(cw.id);
                    }
                }
                break;
            }
        }
    }

    // Apply Open/import outcomes (used by File -> Recent).
    {
        IoManager::OpenEvent ev;
        while (io_manager.TakeLastOpenEvent(ev))
        {
            if (ev.kind == IoManager::OpenEventKind::Canvas && !ev.path.empty())
                push_recent(ev.path);
        }
    }

    // File IO feedback (success/error).
    auto should_apply_placement = [&](const char* window_name) -> bool
    {
        if (!window_name || !*window_name)
            return false;
        return st.applied_imgui_placements.emplace(window_name).second;
    };
    io_manager.RenderStatusWindows(&session_state, should_apply_placement("File Error"));

    // Export dialog (tabbed).
    export_dialog.Render("Export", window, file_dialogs, io_manager, active_canvas,
                         &session_state, should_apply_placement("Export"));

    appui::HandleKeybindings(window, keybinds, session_state,
                             io_manager, file_dialogs, export_dialog,
                             tool_palette, st.tools.compile_tool_script, st.tools.sync_tool_stack,
                             focused_canvas, focused_canvas_window,
                             active_canvas, active_canvas_window,
                             st.done, window_fullscreen, show_minimap_window,
                             show_settings_window, settings_window,
                             fg_color, bg_color,
                             create_new_canvas);

    // Optional: keep the ImGui demo available for reference
    if (show_demo_window)
        ImGui::ShowDemoWindow(&show_demo_window);

    // Unicode Character Picker window
    if (show_character_picker_window)
    {
        const char* name = "Unicode Character Picker";
        character_picker.Render(name, &show_character_picker_window,
                                &session_state, should_apply_placement(name));
    }

    // If the picker selection changed, update the palette's selected cell (replace or select).
    {
        uint32_t cp = 0;
        if (character_picker.TakeSelectionChanged(cp))
        {
            character_palette.OnPickerSelectedCodePoint(cp);
            character_sets.OnExternalSelectedCodePoint(cp);
            tool_brush_cp = cp;
            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
        }
    }

    // Character Palette window
    if (show_character_palette_window)
    {
        const char* name = "Character Palette";
        character_palette.Render(name, &show_character_palette_window,
                                 &session_state, should_apply_placement(name),
                                 active_canvas);
    }

    // If the user clicked a glyph in the palette:
    {
        GlyphToken g;
        std::string utf8;
        if (character_palette.TakeUserSelectionChanged(g, utf8))
        {
            if (g.IsUnicode())
            {
                const uint32_t cp = g.value;
                character_picker.JumpToCodePoint(cp);
                character_sets.OnExternalSelectedCodePoint(cp);
                tool_brush_cp = cp;
                // Use the palette's stored UTF-8 string directly (supports multi-codepoint glyphs,
                // and avoids any encode/decode mismatch).
                tool_brush_utf8 = (!utf8.empty()) ? utf8 : ansl::utf8::encode((char32_t)tool_brush_cp);
            }
            else
            {
                // Embedded glyph index: stored on the canvas as PUA (U+E000 + index).
                const uint32_t cp = (uint32_t)g.ToCanvasCodePoint();
                character_sets.OnExternalSelectedCodePoint(cp);
                tool_brush_cp = cp;
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            }
        }
    }

    // Character Sets window
    if (show_character_sets_window)
    {
        const char* name = "Character Sets";
        character_sets.Render(name, &show_character_sets_window,
                              &session_state, should_apply_placement(name),
                              active_canvas);
    }

    // If the user clicked a slot in the character sets:
    {
        uint32_t cp = 0;
        if (character_sets.TakeUserSelectionChanged(cp))
        {
            character_picker.JumpToCodePoint(cp);
            character_palette.OnPickerSelectedCodePoint(cp);
            tool_brush_cp = cp;
            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
        }
    }

    namespace fs = std::filesystem;

    // Centralized "insert a codepoint at the caret" helper (shared by picker/palette + character sets + hotkeys).
    // Some callers want "typewriter" caret advance; others (Character Sets) want a stationary caret.
    auto insert_cp_into_canvas = [&](AnsiCanvas* dst, uint32_t cp, bool advance_caret)
    {
        if (!dst)
            return;
        if (cp == 0)
            return;

        // Respect current editor FG/BG selection (xterm-256 picker).
        // Canvas uses Color32 where 0 means "unset"; we always apply explicit colours here.
        auto to_idx = [](const ImVec4& c) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);
            return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                         (std::uint8_t)std::clamp(g, 0, 255),
                                         (std::uint8_t)std::clamp(b, 0, 255));
        };
        const AnsiCanvas::Color32 fg32 = (AnsiCanvas::Color32)xterm256::Color32ForIndex(to_idx(fg_color));
        const AnsiCanvas::Color32 bg32 = (AnsiCanvas::Color32)xterm256::Color32ForIndex(to_idx(bg_color));

        int caret_x = 0;
        int caret_y = 0;
        dst->GetCaretCell(caret_x, caret_y);

        // Create an undo boundary before mutating so Undo restores the previous state.
        dst->PushUndoSnapshot();

        const int layer_index = dst->GetActiveLayerIndex();
        dst->SetLayerCell(layer_index, caret_y, caret_x, (char32_t)cp, fg32, bg32);

        if (advance_caret)
        {
            // Advance caret like a simple editor (wrap to next row).
            const int cols = dst->GetColumns();
            int nx = caret_x + 1;
            int ny = caret_y;
            if (cols > 0 && nx >= cols)
            {
                nx = 0;
                ny = caret_y + 1;
            }
            dst->SetCaretCell(nx, ny);
        }
    };

    // Hotkeys for character sets:
    if (focused_canvas)
    {
        const bool any_popup =
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (!any_popup)
        {
            kb::EvalContext kctx;
            kctx.global = true;
            kctx.editor = true;
            kctx.canvas = true;
            kctx.selection = focused_canvas->HasSelection();
            kctx.platform = kb::RuntimePlatform();

            for (int i = 0; i < 12; ++i)
            {
                const std::string id = "charset.insert.f" + std::to_string(i + 1);
                if (keybinds.ActionPressed(id, kctx))
                {
                    character_sets.SelectSlot(i);
                    const uint32_t cp = character_sets.GetSlotCodePoint(i);
                    insert_cp_into_canvas(focused_canvas, cp, /*advance_caret=*/false);
                }
            }

            for (int d = 1; d <= 9; ++d)
            {
                const std::string id = "charset.insert.ctrl_" + std::to_string(d);
                if (keybinds.ActionPressed(id, kctx))
                {
                    const int slot = d - 1;
                    character_sets.SelectSlot(slot);
                    const uint32_t cp = character_sets.GetSlotCodePoint(slot);
                    insert_cp_into_canvas(focused_canvas, cp, /*advance_caret=*/false);
                }
            }
            if (keybinds.ActionPressed("charset.insert.ctrl_0", kctx))
            {
                character_sets.SelectSlot(9);
                const uint32_t cp = character_sets.GetSlotCodePoint(9);
                insert_cp_into_canvas(focused_canvas, cp, /*advance_caret=*/false);
            }
        }
    }

    // Double-click in picker/palette inserts the glyph into the active canvas at the caret.
    {
        uint32_t cp = 0;
        if (character_picker.TakeDoubleClicked(cp))
        {
            insert_cp_into_canvas(active_canvas, cp, /*advance_caret=*/true);
        }
        else
        {
            GlyphToken g;
            if (character_palette.TakeUserDoubleClicked(g))
                insert_cp_into_canvas(active_canvas, (uint32_t)g.ToCanvasCodePoint(), /*advance_caret=*/true);
        }
    }

    // Double-click in the Character Sets window inserts the mapped glyph into the active canvas.
    {
        uint32_t cp = 0;
        if (character_sets.TakeInsertRequested(cp))
            insert_cp_into_canvas(active_canvas, cp, /*advance_caret=*/false);
    }

    // Colour picker showcase window
    if (show_color_picker_window)
    {
        const char* name = "Colour Picker";
        ApplyImGuiWindowPlacement(session_state, name, should_apply_placement(name));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, name);
        const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, name);
        ImGui::Begin("Colour Picker", &show_color_picker_window, flags);
        CaptureImGuiWindowPlacement(session_state, name);
        ApplyImGuiWindowChromeZOrder(&session_state, name);
        RenderImGuiWindowChromeMenu(&session_state, name);

        static int                 last_palette_index = -1;
        static std::vector<ImVec4> saved_palette;

        if (!palettes_error.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Palette load error: %s", palettes_error.c_str());
        }

        // If the active canvas has a stored palette title, sync the picker to it when switching canvases.
        {
            static int last_synced_canvas_id = -1;
            if (active_canvas && !palettes.empty() && last_active_canvas_id != last_synced_canvas_id)
            {
                last_synced_canvas_id = last_active_canvas_id;
                const std::string& want = active_canvas->GetColourPaletteTitle();
                if (!want.empty())
                {
                    for (int i = 0; i < (int)palettes.size(); ++i)
                    {
                        if (palettes[i].title == want)
                        {
                            xterm_selected_palette = i;
                            break;
                        }
                    }
                }
            }
        }

        // Foreground / Background selector at the top (centered).
        {
            float sz     = ImGui::GetFrameHeight() * 2.0f;
            float offset = sz * 0.35f;
            float pad    = 2.0f;
            float widget_width = sz + offset + pad;

            float avail = ImGui::GetContentRegionAvail().x;
            float indent = (avail > widget_width) ? (avail - widget_width) * 0.5f : 0.0f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            const bool fb_widget_changed =
                ImGui::XtermForegroundBackgroundWidget("🙿", fg_color, bg_color, active_fb);
            if (fb_widget_changed)
                xterm_picker_preview_fb = active_fb;
        }

        ImGui::Separator();

        // Picker mode combo (Hue Bar / Hue Wheel) and picker UI
        const char* picker_items[] = { "Hue Bar", "Hue Wheel" };
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##Mode", &xterm_picker_mode, picker_items, IM_ARRAYSIZE(picker_items));

        ImGui::Separator();

        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImVec4& preview_col = (xterm_picker_preview_fb == 0) ? fg_color : bg_color;
        float   picker_col[4] = { preview_col.x, preview_col.y, preview_col.z, preview_col.w };
        bool    value_changed = false;
        bool    used_right = false;
        if (xterm_picker_mode == 0)
            value_changed = ImGui::ColorPicker4_Xterm256_HueBar("##picker", picker_col, false,
                                                               &used_right, &xterm_picker_last_hue,
                                                               saved_palette.data(), (int)saved_palette.size());
        else
            value_changed = ImGui::ColorPicker4_Xterm256_HueWheel("##picker", picker_col, false,
                                                                 &used_right, &xterm_picker_last_hue,
                                                                 saved_palette.data(), (int)saved_palette.size());

        if (value_changed)
        {
            int dst_fb = used_right ? (1 - active_fb) : active_fb;
            xterm_picker_preview_fb = dst_fb;
            ImVec4& dst = (dst_fb == 0) ? fg_color : bg_color;
            dst.x = picker_col[0];
            dst.y = picker_col[1];
            dst.z = picker_col[2];
            dst.w = picker_col[3];
        }
        ImGui::EndGroup();

        ImGui::Separator();

        // Palette selection combo
        {
            std::vector<const char*> names;
            names.reserve(palettes.size());
            for (const auto& p : palettes)
                names.push_back(p.title.c_str());

            if (!names.empty())
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::Combo("##Palette", &xterm_selected_palette, names.data(), (int)names.size());
            }
        }

        // Rebuild working palette when selection changes.
        if (xterm_selected_palette != last_palette_index && !palettes.empty())
        {
            saved_palette = palettes[xterm_selected_palette].colors;
            last_palette_index = xterm_selected_palette;
            if (active_canvas && xterm_selected_palette >= 0 && xterm_selected_palette < (int)palettes.size())
                active_canvas->SetColourPaletteTitle(palettes[xterm_selected_palette].title);
        }

        ImGui::BeginGroup();

        const ImGuiStyle& style = ImGui::GetStyle();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const int count = (int)saved_palette.size();

        int   best_cols = 1;
        float best_size = 0.0f;

        if (count > 0 && avail.x > 0.0f)
        {
            for (int cols = 1; cols <= count; ++cols)
            {
                float total_spacing_x = style.ItemSpacing.x * (cols - 1);
                float width_limit = (avail.x - total_spacing_x) / (float)cols;
                if (width_limit <= 0.0f)
                    break;

                int rows = (count + cols - 1) / cols;

                float button_size = width_limit;
                if (avail.y > 0.0f)
                {
                    float total_spacing_y = style.ItemSpacing.y * (rows - 1);
                    float height_limit = (avail.y - total_spacing_y) / (float)rows;
                    if (height_limit <= 0.0f)
                        continue;
                    button_size = std::min(width_limit, height_limit);
                }

                if (button_size > best_size)
                {
                    best_size = button_size;
                    best_cols = cols;
                }
            }

            if (best_size <= 0.0f)
            {
                best_cols = 1;
                best_size = style.FramePadding.y * 2.0f + 8.0f;
            }
        }

        const int cols = (count > 0) ? best_cols : 1;
        ImVec2 button_size(best_size, best_size);

        ImVec4& pal_primary   = (active_fb == 0) ? fg_color : bg_color;
        ImVec4& pal_secondary = (active_fb == 0) ? bg_color : fg_color;

        for (int n = 0; n < count; n++)
        {
            ImGui::PushID(n);
            if (n % cols != 0)
                ImGui::SameLine(0.0f, style.ItemSpacing.y);

            ImGuiColorEditFlags palette_button_flags =
                ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
            bool left_clicked = ImGui::ColorButton("##palette", saved_palette[n],
                                                   palette_button_flags, button_size);
            if (left_clicked)
            {
                pal_primary.x = saved_palette[n].x;
                pal_primary.y = saved_palette[n].y;
                pal_primary.z = saved_palette[n].z;
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                pal_secondary.x = saved_palette[n].x;
                pal_secondary.y = saved_palette[n].y;
                pal_secondary.z = saved_palette[n].z;
            }

            ImGui::PopID();
        }

        ImGui::EndGroup();

        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
    }

    // Tool Palette window
    if (show_tool_palette_window)
    {
        const char* name = "Tool Palette";
        const bool tool_palette_changed =
            tool_palette.Render(name, &show_tool_palette_window,
                                &session_state, should_apply_placement(name));
        (void)tool_palette_changed;

        if (tool_palette.TakeReloadRequested())
        {
            std::string err;
            if (!tool_palette.LoadFromDirectory(tool_palette.GetToolsDir().empty() ? PhosphorAssetPath("tools") : tool_palette.GetToolsDir(), err))
                tools_error = err;
            else
                tools_error.clear();
        }

        std::string tool_path;
        if (tool_palette.TakeActiveToolChanged(tool_path))
        {
            if (st.tools.compile_tool_script) st.tools.compile_tool_script(tool_path);
            if (st.tools.sync_tool_stack) st.tools.sync_tool_stack();
        }

        if (!tool_compile_error.empty())
        {
            const char* wname = "Tool Error";
            ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
            ImGui::Begin("Tool Error", nullptr, flags);
            CaptureImGuiWindowPlacement(session_state, wname);
            ApplyImGuiWindowChromeZOrder(&session_state, wname);
            RenderImGuiWindowChromeMenu(&session_state, wname);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tool_compile_error.c_str());
            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }

        if (!tools_error.empty())
        {
            const char* wname = "Tools Error";
            ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
            ImGui::Begin("Tools Error", nullptr, flags);
            CaptureImGuiWindowPlacement(session_state, wname);
            ApplyImGuiWindowChromeZOrder(&session_state, wname);
            RenderImGuiWindowChromeMenu(&session_state, wname);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tools_error.c_str());
            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }

        // Tool parameters UI (settings.params -> ctx.params)
        if (tool_engine.HasParams())
        {
            const char* wname = "Tool Parameters";
            ApplyImGuiWindowPlacement(session_state, wname, should_apply_placement(wname));
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_AlwaysAutoResize | GetImGuiWindowChromeExtraFlags(session_state, wname);
            const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, wname);
            ImGui::Begin("Tool Parameters", nullptr, flags);
            CaptureImGuiWindowPlacement(session_state, wname);
            ApplyImGuiWindowChromeZOrder(&session_state, wname);
            RenderImGuiWindowChromeMenu(&session_state, wname);
            const ToolSpec* t = tool_palette.GetActiveTool();
            if (t)
                ImGui::Text("%s", t->label.c_str());
            ImGui::Separator();
            (void)RenderAnslParamsUI("tool_params", tool_engine);
            ImGui::End();
            PopImGuiWindowChromeAlpha(alpha_pushed);
        }
    }

    // Render each canvas window
    for (size_t i = 0; i < canvases.size(); ++i)
    {
        if (!canvases[i])
            continue;
        CanvasWindow& canvas = *canvases[i];
        char close_popup_id[96];
        std::snprintf(close_popup_id, sizeof(close_popup_id),
                      "Save changes?##close_canvas_%d", canvas.id);

        auto queue_close = [&]() {
            canvas.open = false;
            close_canvas_ids.push_back(canvas.id);
        };

        auto request_close = [&]() {
            if (canvas.canvas.IsModifiedSinceLastSave())
            {
                // Veto the close, re-open, and ask.
                canvas.open = true;
                canvas.close_modal_open = true;
                ImGui::OpenPopup(close_popup_id);
            }
            else
            {
                queue_close();
            }
        };

        // Close requested via keybinding (window not rendered) or earlier frame state.
        if (!canvas.open && !canvas.close_modal_open && !canvas.close_waiting_on_save)
        {
            request_close();
            if (!canvas.open)
                continue;
        }
        // If a Save dialog is in flight for this canvas, keep it alive regardless of close attempts.
        if (!canvas.open && canvas.close_waiting_on_save)
            canvas.open = true;

        auto sanitize_imgui_id = [](std::string s) -> std::string
        {
            for (;;)
            {
                const size_t pos = s.find("##");
                if (pos == std::string::npos)
                    break;
                s.replace(pos, 2, "#");
            }
            return s;
        };

        std::string canvas_path;
        if (canvas.canvas.HasFilePath())
            canvas_path = canvas.canvas.GetFilePath();
        else if (!canvas.restore_phos_cache_rel.empty())
            canvas_path = PhosphorCachePath(canvas.restore_phos_cache_rel);
        else
        {
            const std::string rel = "session_canvases/canvas_" + std::to_string(canvas.id) + ".phos";
            canvas_path = PhosphorCachePath(rel);
        }

        // Dirty indicator: affect only the visible part of the title, not the ImGui ID (after ##).
        const std::string canvas_id = "canvas:" + sanitize_imgui_id(canvas_path) + "#" + std::to_string(canvas.id);
        const bool dirty = canvas.canvas.IsModifiedSinceLastSave();
        std::string title = (dirty ? "* " : "") + canvas_path + "##" + canvas_id;

        const auto it = session_state.imgui_windows.find(title);
        const bool has_saved = (it != session_state.imgui_windows.end() && it->second.valid);

        // First-time placement sizing block is unchanged from main.cpp.
        if (!has_saved)
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 work_pos  = vp ? vp->WorkPos  : ImVec2(0, 0);
            const ImVec2 work_size = vp ? vp->WorkSize : ImVec2(1280, 720);
            const ImVec2 center(work_pos.x + work_size.x * 0.5f,
                                work_pos.y + work_size.y * 0.5f);

            const float font_size = ImGui::GetFontSize();
            const float base_cell_w = std::max(1.0f, font_size * 0.5f);
            const float base_cell_h = std::max(1.0f, font_size);
            const float zoom = canvas.canvas.GetZoom();

            float snapped_cell_w = std::floor(base_cell_w * zoom + 0.5f);
            if (snapped_cell_w < 1.0f) snapped_cell_w = 1.0f;
            const float snapped_scale = (base_cell_w > 0.0f) ? (snapped_cell_w / base_cell_w) : 1.0f;
            float scaled_cell_w = snapped_cell_w;
            float scaled_cell_h = std::floor(base_cell_h * snapped_scale + 0.5f);
            if (scaled_cell_h < 1.0f) scaled_cell_h = 1.0f;

            const int cols2 = canvas.canvas.GetColumns();
            const int rows2 = canvas.canvas.GetRows();
            const ImVec2 grid_px(scaled_cell_w * (float)cols2,
                                 scaled_cell_h * (float)rows2);

            const float status_h =
                std::max(ImGui::GetTextLineHeightWithSpacing(),
                         ImGui::GetFrameHeightWithSpacing());

            const ImVec2 window_pad(0.0f, 0.0f);
            ImVec2 desired(grid_px.x + window_pad.x * 2.0f + 2.0f,
                           status_h + grid_px.y + window_pad.y * 2.0f + 2.0f);

            const float margin = 40.0f;
            const ImVec2 max_sz(std::max(200.0f, work_size.x - margin),
                                std::max(150.0f, work_size.y - margin));
            if (desired.x > max_sz.x) desired.x = max_sz.x;
            if (desired.y > max_sz.y) desired.y = max_sz.y;

            const float offset = 18.0f * (float)((canvas.id - 1) % 10);
            const ImVec2 pos(center.x + offset, center.y + offset);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(desired, ImGuiCond_Appearing);
        }

        ApplyImGuiWindowPlacement(session_state, title.c_str(),
                                  has_saved && should_apply_placement(title.c_str()));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, title.c_str());
        const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, title.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool open_before_begin = canvas.open;
        ImGui::Begin(title.c_str(), &canvas.open, flags);
        CaptureImGuiWindowPlacement(session_state, title.c_str());
        ApplyImGuiWindowChromeZOrder(&session_state, title.c_str());
        RenderImGuiWindowChromeMenu(&session_state, title.c_str());

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            last_active_canvas_id = canvas.id;

        {
            const bool any_click =
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
            if (any_click && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
                last_active_canvas_id = canvas.id;
        }

        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "canvas_%d", canvas.id);

        auto to_idx = [](const ImVec4& c) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);
            return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                         (std::uint8_t)std::clamp(g, 0, 255),
                                         (std::uint8_t)std::clamp(b, 0, 255));
        };
        const int fg_idx = to_idx(fg_color);
        const int bg_idx = to_idx(bg_color);

        auto tool_runner = [&](AnsiCanvas& c, int phase) {
            if (!tool_engine.HasRenderFunction())
                return;

            AnslFrameContext ctx;
            std::vector<int> palette_xterm;
            std::vector<ToolCommand> commands;
            ToolCommandSink cmd_sink;
            cmd_sink.allow_tool_commands = true;
            cmd_sink.out_commands = &commands;
            ctx.cols = c.GetColumns();
            ctx.rows = c.GetRows();
            ctx.frame = st.frame_counter;
            ctx.time = ImGui::GetTime() * 1000.0;
            ctx.metrics_aspect = c.GetLastCellAspect();
            ctx.phase = phase;
            ctx.focused = c.HasFocus();
            ctx.fg = fg_idx;
            ctx.bg = bg_idx;
            ctx.brush_utf8 = tool_brush_utf8;
            ctx.brush_cp = (int)tool_brush_cp;
            ctx.palette_xterm = nullptr;
            ctx.allow_caret_writeback = true;

            c.GetCaretCell(ctx.caret_x, ctx.caret_y);

            // Active palette: expose allowed xterm indices to tools (for quantization/snapping).
            // Prefer the canvas's stored palette title; fallback to the current global selection.
            if (!palettes.empty())
            {
                const ColourPaletteDef* def = nullptr;
                const std::string& want = c.GetColourPaletteTitle();
                if (!want.empty())
                {
                    for (const auto& p : palettes)
                    {
                        if (p.title == want)
                        {
                            def = &p;
                            break;
                        }
                    }
                }
                if (!def && xterm_selected_palette >= 0 && xterm_selected_palette < (int)palettes.size())
                    def = &palettes[(size_t)xterm_selected_palette];
                if (!def)
                    def = &palettes[0];

                std::unordered_set<int> seen;
                seen.reserve(def->colors.size());
                for (const ImVec4& ccol : def->colors)
                {
                    const int r = (int)std::lround(ccol.x * 255.0f);
                    const int g = (int)std::lround(ccol.y * 255.0f);
                    const int b = (int)std::lround(ccol.z * 255.0f);
                    const int idx = xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                                          (std::uint8_t)std::clamp(g, 0, 255),
                                                          (std::uint8_t)std::clamp(b, 0, 255));
                    if (seen.insert(idx).second)
                        palette_xterm.push_back(idx);
                }
                if (!palette_xterm.empty())
                    ctx.palette_xterm = &palette_xterm;
            }

            int cx = 0, cy = 0, half_y = 0, px = 0, py = 0, phalf_y = 0;
            bool l = false, r = false, pl = false, pr = false;
            ctx.cursor_valid = c.GetCursorCell(cx, cy, half_y, l, r, px, py, phalf_y, pl, pr);
            ctx.cursor_x = cx;
            ctx.cursor_y = cy;
            ctx.cursor_half_y = half_y;
            ctx.cursor_left_down = l;
            ctx.cursor_right_down = r;
            ctx.cursor_px = px;
            ctx.cursor_py = py;
            ctx.cursor_phalf_y = phalf_y;
            ctx.cursor_prev_left_down = pl;
            ctx.cursor_prev_right_down = pr;

            std::vector<char32_t> typed;
            std::vector<std::string> pressed_actions;
            ctx.actions_pressed = nullptr;
            if (phase == 0)
            {
                c.TakeTypedCodepoints(typed);
                ctx.typed = &typed;

                const auto keys = c.TakeKeyEvents();
                ctx.key_left = keys.left;
                ctx.key_right = keys.right;
                ctx.key_up = keys.up;
                ctx.key_down = keys.down;
                ctx.key_home = keys.home;
                ctx.key_end = keys.end;
                ctx.key_backspace = keys.backspace;
                ctx.key_delete = keys.del;
                ctx.key_enter = keys.enter;

                ctx.key_c = keys.c;
                ctx.key_v = keys.v;
                ctx.key_x = keys.x;
                ctx.key_a = keys.a;
                ctx.key_escape = keys.escape;

                ImGuiIO& io = ImGui::GetIO();
                ctx.mod_ctrl = io.KeyCtrl;
                ctx.mod_shift = io.KeyShift;
                ctx.mod_alt = io.KeyAlt;
                ctx.mod_super = io.KeySuper;

                kb::EvalContext kctx;
                kctx.global = true;
                kctx.editor = c.HasFocus();
                kctx.canvas = c.HasFocus();
                kctx.selection = c.HasSelection();
                kctx.platform = kb::RuntimePlatform();

                const kb::Hotkeys hk = keybinds.EvalCommonHotkeys(kctx);
                ctx.hotkeys.copy = hk.copy;
                ctx.hotkeys.cut = hk.cut;
                ctx.hotkeys.paste = hk.paste;
                ctx.hotkeys.selectAll = hk.select_all;
                ctx.hotkeys.cancel = hk.cancel;
                ctx.hotkeys.deleteSelection = hk.delete_selection;

                pressed_actions.clear();
                if (hk.copy) pressed_actions.push_back("edit.copy");
                if (hk.cut) pressed_actions.push_back("edit.cut");
                if (hk.paste) pressed_actions.push_back("edit.paste");
                if (hk.select_all) pressed_actions.push_back("edit.select_all");
                if (hk.cancel) pressed_actions.push_back("selection.clear_or_cancel");
                if (hk.delete_selection) pressed_actions.push_back("selection.delete");

                if (active_tool_id() == "select")
                {
                    auto push_if_pressed = [&](std::string_view id) {
                        if (keybinds.ActionPressed(id, kctx))
                            pressed_actions.push_back(std::string(id));
                    };
                    push_if_pressed("selection.op.rotate_cw");
                    push_if_pressed("selection.op.flip_x");
                    push_if_pressed("selection.op.flip_y");
                    push_if_pressed("selection.op.center");
                    push_if_pressed("selection.crop");
                }
                ctx.actions_pressed = &pressed_actions;
            }

            std::string err;
            if (!tool_engine.RunFrame(c, c.GetActiveLayerIndex(), ctx, cmd_sink, false, err))
            {
                tool_compile_error = err;
            }
            else
            {
                auto apply_idx_to_color = [&](int idx, ImVec4& dst) {
                    idx = std::clamp(idx, 0, 255);
                    const xterm256::Rgb rgb = xterm256::RgbForIndex(idx);
                    dst.x = (float)rgb.r / 255.0f;
                    dst.y = (float)rgb.g / 255.0f;
                    dst.z = (float)rgb.b / 255.0f;
                    dst.w = 1.0f;
                };

                for (const ToolCommand& cmd : commands)
                {
                    switch (cmd.type)
                    {
                    case ToolCommand::Type::BrushSet:
                    {
                        if (cmd.brush_cp > 0)
                        {
                            const uint32_t cp = cmd.brush_cp;
                            character_picker.JumpToCodePoint(cp);
                            tool_brush_cp = cp;
                            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
                            character_palette.OnPickerSelectedCodePoint(cp);
                            character_sets.OnExternalSelectedCodePoint(cp);
                        }
                    } break;
                    case ToolCommand::Type::PaletteSet:
                    {
                        if (cmd.has_fg)
                            apply_idx_to_color(cmd.fg, fg_color);
                        if (cmd.has_bg)
                            apply_idx_to_color(cmd.bg, bg_color);
                    } break;
                    case ToolCommand::Type::ToolActivatePrev:
                    {
                        activate_prev_tool();
                    } break;
                    case ToolCommand::Type::ToolActivate:
                    {
                        activate_tool_by_id(cmd.tool_id);
                    } break;
                    case ToolCommand::Type::CanvasCropToSelection:
                    {
                        if (c.IsMovingSelection())
                            (void)c.CommitMoveSelection();
                        if (!c.HasSelection())
                            break;

                        const AnsiCanvas::Rect r = c.GetSelectionRect();
                        if (r.w <= 0 || r.h <= 0)
                            break;

                        struct CropCell
                        {
                            char32_t cp = U' ';
                            AnsiCanvas::Color32 fg = 0;
                            AnsiCanvas::Color32 bg = 0;
                        };

                        const int layer_count = c.GetLayerCount();
                        std::vector<std::vector<CropCell>> saved;
                        saved.resize(std::max(0, layer_count));
                        const size_t n = (size_t)r.w * (size_t)r.h;
                        for (int li = 0; li < layer_count; ++li)
                        {
                            saved[(size_t)li].assign(n, CropCell{});
                            for (int y = 0; y < r.h; ++y)
                                for (int x = 0; x < r.w; ++x)
                                {
                                    const int sx = r.x + x;
                                    const int sy = r.y + y;
                                    const size_t idx = (size_t)y * (size_t)r.w + (size_t)x;
                                    CropCell cell;
                                    cell.cp = c.GetLayerCell(li, sy, sx);
                                    (void)c.GetLayerCellColors(li, sy, sx, cell.fg, cell.bg);
                                    saved[(size_t)li][idx] = cell;
                                }
                        }

                        c.SetColumns(r.w);
                        c.SetRows(r.h);

                        for (int li = 0; li < layer_count; ++li)
                        {
                            (void)c.ClearLayer(li, U' ');
                            const std::vector<CropCell>& cells = saved[(size_t)li];
                            for (int y = 0; y < r.h; ++y)
                                for (int x = 0; x < r.w; ++x)
                                {
                                    const size_t idx = (size_t)y * (size_t)r.w + (size_t)x;
                                    if (idx >= cells.size())
                                        continue;
                                    const CropCell& cell = cells[idx];
                                    if (cell.fg != 0 || cell.bg != 0)
                                        (void)c.SetLayerCell(li, y, x, cell.cp, cell.fg, cell.bg);
                                    else
                                        (void)c.SetLayerCell(li, y, x, cell.cp);
                                }
                        }

                        c.SetSelectionCorners(0, 0, r.w - 1, r.h - 1);
                    } break;
                    }
                }
            }
        };

        const bool bg_before = canvas.canvas.IsCanvasBackgroundWhite();
        canvas.canvas.Render(id_buf, tool_runner);
        const bool bg_after = canvas.canvas.IsCanvasBackgroundWhite();
        if (bg_after != bg_before)
            session_state.canvas_bg_white = bg_after;

        if (canvas.canvas.TakeFocusGained())
        {
            last_active_canvas_id = canvas.id;
            for (auto& other_ptr : canvases)
            {
                if (!other_ptr)
                    continue;
                CanvasWindow& other = *other_ptr;
                if (!other.open)
                    continue;
                if (other.id == canvas.id)
                    continue;
                other.canvas.ClearFocus();
            }
        }

        if (canvas.canvas.TakeOpenSauceEditorRequest())
            canvas.sauce_dialog.OpenFromCanvas(canvas.canvas);

        char sauce_popup_id[64];
        snprintf(sauce_popup_id, sizeof(sauce_popup_id), "Edit SAUCE##sauce_%d", canvas.id);
        canvas.sauce_dialog.Render(canvas.canvas, sauce_popup_id);

        ImGui::End();
        ImGui::PopStyleVar();
        PopImGuiWindowChromeAlpha(alpha_pushed);

        // Window close button: ImGui flips `canvas.open` to false; intercept here.
        if (open_before_begin && !canvas.open && !canvas.close_waiting_on_save)
            request_close();

        // Close-confirm modal (Save / Don't Save / Cancel).
        {
            // Ensure consistent modal placement (center of the application viewport).
            if (ImGuiViewport* vp = ImGui::GetMainViewport())
                ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal(close_popup_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool has_path = canvas.canvas.HasFilePath();
            const std::string& path = canvas.canvas.GetFilePath();

            if (has_path)
                ImGui::Text("Save changes to:");
            else
                ImGui::Text("Save changes to this canvas?");
            if (has_path)
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", path.c_str());
            }
            ImGui::Separator();

            if (ImGui::Button("Save"))
            {
                canvas.close_modal_open = false;
                canvas.close_waiting_on_save = true;
                io_manager.SaveProject(window, file_dialogs, &canvas.canvas);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save"))
            {
                canvas.close_modal_open = false;
                queue_close();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                canvas.close_modal_open = false;
                canvas.open = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // Apply any queued canvas closes (and delete session cache files so they don't become orphaned).
    if (!close_canvas_ids.empty())
    {
        auto should_close = [&](int id) -> bool {
            for (int cid : close_canvas_ids)
                if (cid == id) return true;
            return false;
        };

        for (const int cid : close_canvas_ids)
        {
            for (const auto& cwptr : canvases)
            {
                if (!cwptr)
                    continue;
                const CanvasWindow& cw = *cwptr;
                if (cw.id != cid)
                    continue;

                // Delete the session cache file for this canvas (if any).
                const std::string rel = !cw.restore_phos_cache_rel.empty()
                    ? cw.restore_phos_cache_rel
                    : ("session_canvases/canvas_" + std::to_string(cw.id) + ".phos");
                std::string derr;
                (void)open_canvas_cache::DeleteSessionCanvasCachePhos(rel, derr);
                break;
            }
        }

        if (should_close(last_active_canvas_id))
            last_active_canvas_id = -1;

        canvases.erase(std::remove_if(canvases.begin(), canvases.end(),
                                      [&](const std::unique_ptr<CanvasWindow>& cwptr) {
                                          return cwptr && should_close(cwptr->id);
                                      }),
                      canvases.end());
    }

    // Layer Manager window
    if (show_layer_manager_window)
    {
        const char* name = "Layer Manager";
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        layer_manager.Render(name, &show_layer_manager_window, ui_active_canvas,
                             &session_state, should_apply_placement(name));
    }

    // ANSL Editor window
    if (show_ansl_editor_window)
    {
        const char* name = "ANSL Editor";
        ApplyImGuiWindowPlacement(session_state, name, should_apply_placement(name));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, name);
        const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, name);
        ImGui::Begin("ANSL Editor", &show_ansl_editor_window, flags);
        CaptureImGuiWindowPlacement(session_state, name);
        ApplyImGuiWindowChromeZOrder(&session_state, name);
        RenderImGuiWindowChromeMenu(&session_state, name);
        auto to_idx = [](const ImVec4& c) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);
            return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                         (std::uint8_t)std::clamp(g, 0, 255),
                                         (std::uint8_t)std::clamp(b, 0, 255));
        };
        const int fg_idx2 = to_idx(fg_color);
        const int bg_idx2 = to_idx(bg_color);
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        ansl_editor.Render("ansl_editor", ui_active_canvas, ansl_engine, fg_idx2, bg_idx2, ImGuiInputTextFlags_AllowTabInput);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
    }

    // Render each imported image window
    for (size_t i = 0; i < images.size(); ++i)
    {
        ImageWindow& img = images[i];
        if (!img.open)
            continue;

        std::string img_path = img.path.empty()
            ? ("untitled://image/" + std::to_string(img.id))
            : img.path;
        for (;;)
        {
            const size_t pos = img_path.find("##");
            if (pos == std::string::npos)
                break;
            img_path.replace(pos, 2, "#");
        }
        const std::string img_id = "image:" + img_path + "#" + std::to_string(img.id);
        std::string title = img_path + "##" + img_id;

        RenderImageWindow(title.c_str(), img, image_to_chafa_dialog,
                          &session_state, should_apply_placement(title.c_str()));
    }

    // Minimap window
    if (show_minimap_window)
    {
        const char* name = "Minimap";
        preview_texture.Update(active_canvas, 768, ImGui::GetTime());
        const CanvasPreviewTextureView pv_view = preview_texture.View();
        minimap_window.Render(name, &show_minimap_window, active_canvas, &pv_view,
                              &session_state, should_apply_placement(name));
    }

    // 16colo.rs browser window.
    if (show_16colors_browser_window)
    {
        const char* name = "16colo.rs Browser";
        SixteenColorsBrowserWindow::Callbacks cbs;
        cbs.create_canvas = [&](AnsiCanvas&& c)
        {
            auto canvas_window = std::make_unique<CanvasWindow>();
            canvas_window->open = true;
            canvas_window->id = next_canvas_id++;
            canvas_window->canvas = std::move(c);
            canvas_window->canvas.SetKeyBindingsEngine(&keybinds);
            canvas_window->canvas.SetUndoLimit(session_state.undo_limit);
            canvas_window->canvas.MarkSaved();
            last_active_canvas_id = canvas_window->id;
            canvases.push_back(std::move(canvas_window));
        };
        cbs.create_image = [&](SixteenColorsBrowserWindow::Callbacks::LoadedImage&& li)
        {
            ImageWindow img;
            img.id = next_image_id++;
            img.path = std::move(li.path);
            img.width = li.width;
            img.height = li.height;
            img.pixels = std::move(li.pixels);
            img.open = true;
            images.push_back(std::move(img));
        };
        sixteen_browser.Render(name, &show_16colors_browser_window, cbs,
                               &session_state, should_apply_placement(name));
    }

    // Settings window
    if (show_settings_window)
    {
        const char* name = "Settings";
        settings_window.SetOpen(show_settings_window);
        settings_window.SetUndoLimitApplier([&](size_t limit) {
            session_state.undo_limit = limit;
            for (auto& cptr : canvases)
            {
                if (!cptr || !cptr->open)
                    continue;
                cptr->canvas.SetUndoLimit(limit);
            }
        });
        settings_window.Render(name, &session_state, should_apply_placement(name));
        show_settings_window = settings_window.IsOpen();
    }

    // Chafa conversion UI
    image_to_chafa_dialog.Render(&session_state,
                                 should_apply_placement("Image \xE2\x86\x92 ANSI (Chafa)##chafa_preview"));
    {
        AnsiCanvas converted;
        if (image_to_chafa_dialog.TakeAccepted(converted))
        {
            auto canvas_window = std::make_unique<CanvasWindow>();
            canvas_window->open = true;
            canvas_window->id = next_canvas_id++;
            canvas_window->canvas = std::move(converted);
            canvas_window->canvas.SetKeyBindingsEngine(&keybinds);
            canvas_window->canvas.SetUndoLimit(session_state.undo_limit);
            canvas_window->canvas.MarkSaved();
            last_active_canvas_id = canvas_window->id;
            canvases.push_back(std::move(canvas_window));
        }
    }

    // Enforce pinned z-order globally.
    ApplyImGuiWindowChromeGlobalZOrder(session_state);

    // Autosave / crash recovery:
    // Periodically persist session.json + cached canvas projects so crashes restore recent work.
    {
        const double now_s = ImGui::GetTime();
        const double kAutosaveIntervalS = 30.0;
        if (st.autosave_last_s <= 0.0)
            st.autosave_last_s = now_s;
        if (!st.done && (now_s - st.autosave_last_s) >= kAutosaveIntervalS)
        {
            // Only autosave if there's something worth saving.
            bool any_open = false;
            for (const auto& cptr : canvases)
            {
                if (cptr && cptr->open)
                {
                    any_open = true;
                    break;
                }
            }
            if (any_open)
            {
                workspace_persist::SaveSessionStateOnExit(session_state,
                                                         window,
                                                         io_manager,
                                                         tool_palette,
                                                         ansl_editor,
                                                         show_color_picker_window,
                                                         show_character_picker_window,
                                                         show_character_palette_window,
                                                         show_character_sets_window,
                                                         show_layer_manager_window,
                                                         show_ansl_editor_window,
                                                         show_tool_palette_window,
                                                         show_minimap_window,
                                                         show_settings_window,
                                                         show_16colors_browser_window,
                                                         fg_color,
                                                         bg_color,
                                                         active_fb,
                                                         xterm_picker_mode,
                                                         xterm_selected_palette,
                                                         xterm_picker_preview_fb,
                                                         xterm_picker_last_hue,
                                                         last_active_canvas_id,
                                                         next_canvas_id,
                                                         next_image_id,
                                                         canvases,
                                                         images);
            }
            st.autosave_last_s = now_s;
        }
    }

    // Rendering
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized =
        (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized)
    {
        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;
        vk.FrameRender(wd, draw_data);
        vk.FramePresent(wd);
    }

    st.mouse_down_prev =
        ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        ImGui::IsMouseDown(ImGuiMouseButton_Middle);
}

} // namespace app


