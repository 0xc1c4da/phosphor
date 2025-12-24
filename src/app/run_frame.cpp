#include "app/run_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unordered_map>
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
#include "app/bitmap_glyph_atlas_texture.h"
#include "app/clipboard_utils.h"

#include <cfloat>

#include "core/fonts.h"
#include "core/color_system.h"
#include "core/glyph_resolve.h"
#include "core/paths.h"
#include "core/xterm256_palette.h"

#include "ansl/ansl_native.h"
#include "ansl/ansl_script_engine.h"

#include "io/file_dialog_tags.h"
#include "io/image_loader.h"
#include "io/io_manager.h"
#include "io/formats/gpl.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/open_canvas_cache.h"
#include "io/session/session_state.h"
#include "io/session/imgui_persistence.h"

#include "ui/ansl_editor.h"
#include "ui/ansl_params_ui.h"
#include "ui/brush_palette_window.h"
#include "ui/character_palette.h"
#include "ui/character_picker.h"
#include "ui/character_set.h"
#include "ui/colour_picker.h"
#include "ui/colour_palette.h"
#include "ui/export_dialog.h"
#include "ui/glyph_token.h"
#include "ui/image_to_chafa_dialog.h"
#include "ui/markdown_to_ansi_dialog.h"
#include "ui/image_window.h"
#include "ui/imgui_window_chrome.h"
#include "ui/layer_manager.h"
#include "ui/minimap_window.h"
#include "ui/settings.h"
#include "ui/sixteen_colors_browser.h"
#include "ui/tool_palette.h"
#include "ui/tool_parameters_window.h"
#include "ui/tool_params.h"

#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>

namespace app
{

namespace
{
using nlohmann::json;

struct FallbackToolState
{
    std::unique_ptr<AnslScriptEngine> engine;
    std::string                      last_source;
    std::string                      last_error;
};

static std::string ReadFileToString(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool ToolClaimsAction(const ToolSpec* t, std::string_view action_id)
{
    if (!t)
        return false;
    for (const ToolSpec::HandleRule& r : t->handles)
        if (r.when == ToolSpec::HandleWhen::Active && r.action == action_id)
            return true;
    return false;
}

static bool ToolFallbackClaimsAction(const ToolSpec& t, std::string_view action_id)
{
    for (const ToolSpec::HandleRule& r : t.handles)
        if (r.when == ToolSpec::HandleWhen::Inactive && r.action == action_id)
            return true;
    return false;
}
} // namespace

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
    MarkdownToAnsiDialog& markdown_to_ansi_dialog = *st.ui.markdown_to_ansi_dialog;
    MinimapWindow& minimap_window = *st.ui.minimap_window;
    CanvasPreviewTexture& preview_texture = *st.ui.preview_texture;
    BitmapGlyphAtlasTextureCache& bitmap_glyph_atlas = *st.ui.bitmap_glyph_atlas;
    SixteenColorsBrowserWindow& sixteen_browser = *st.ui.sixteen_browser;
    BrushPaletteWindow& brush_palette = *st.ui.brush_palette_window;

    // Advance the atlas cache clock and collect deferred frees.
    // (Safe to call every frame; no-ops if cache is uninitialized.)
    bitmap_glyph_atlas.BeginFrame();

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
    bool& show_brush_palette_window = *st.toggles.show_brush_palette_window;
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

    std::uint32_t& tool_brush_glyph = *st.tools.tool_brush_glyph;
    std::uint32_t& tool_brush_cp = *st.tools.tool_brush_cp;
    std::string& tool_brush_utf8 = *st.tools.tool_brush_utf8;
    std::uint32_t& tool_attrs_mask = *st.tools.tool_attrs_mask;

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

    // Track which tool id the current `tool_engine` is compiled for (so we can persist params per-tool).
    static std::string s_compiled_tool_id;
    if (s_compiled_tool_id.empty())
        s_compiled_tool_id = active_tool_id();
    static bool s_restored_initial_tool_params = false;
    if (!s_restored_initial_tool_params)
    {
        tool_params::RestoreToolParamsFromSession(session_state, s_compiled_tool_id, tool_engine);
        s_restored_initial_tool_params = true;
    }

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

    bool layer_thumbnails_refresh_release = false;
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

        // Thumbnail refresh heuristic: only refresh expensive layer thumbnails on user interaction boundaries.
        // This intentionally does NOT include mouse motion (dragging) or key down repeats.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_KEY_UP)
            layer_thumbnails_refresh_release = true;

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
        focused_canvas = &c.canvas;
        focused_canvas_window = &c;
        if (last_active_canvas_id == -1)
            last_active_canvas_id = c.id;
        break;
    }
    // Active canvas for global actions (File menu, Edit menu items, future actions):
    // - prefer the last active canvas window (tracks window focus/clicks)
    // - otherwise fall back to focused grid canvas
    // - otherwise fall back to the first open canvas
    AnsiCanvas* active_canvas = nullptr;
    CanvasWindow* active_canvas_window = nullptr;
    if (last_active_canvas_id != -1)
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
    if (!active_canvas && focused_canvas)
    {
        active_canvas = focused_canvas;
        active_canvas_window = focused_canvas_window;
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

    // If the active canvas window changes, switch the global tool brush glyph and keep
    // picker/palette selections synchronized with that canvas' stored active glyph.
    {
        static int s_prev_active_canvas_id = -999999;
        const int cur_id = active_canvas_window ? active_canvas_window->id : -1;
        if (cur_id != s_prev_active_canvas_id)
        {
            s_prev_active_canvas_id = cur_id;
            if (active_canvas)
            {
                tool_brush_glyph = (std::uint32_t)active_canvas->GetActiveGlyph();
                tool_brush_cp = (std::uint32_t)phos::glyph::ToUnicodeRepresentative((phos::GlyphId)tool_brush_glyph);
                if (tool_brush_cp == 0)
                    tool_brush_cp = (std::uint32_t)U' ';
                tool_brush_utf8 = active_canvas->GetActiveGlyphUtf8();
                if (tool_brush_utf8.empty())
                    tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);

                character_picker.RestoreSelectedCodePoint(tool_brush_cp);
                character_palette.SyncSelectionFromActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8, active_canvas);
                character_sets.OnExternalSelectedCodePoint(tool_brush_cp);
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

    // Apply global zoom snapping preference to all open canvases.
    {
        const int mode_i = std::clamp(session_state.zoom_snap_mode, 0, 2);
        const AnsiCanvas::ZoomSnapMode mode = (AnsiCanvas::ZoomSnapMode)mode_i;
        for (auto& cptr : canvases)
        {
            if (!cptr || !cptr->open)
                continue;
            cptr->canvas.SetZoomSnapMode(mode);
        }
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
        // Restored cached projects should be "clean" until the user edits.
        cw.canvas.MarkSaved();
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
        canvas_window->canvas.SetBitmapGlyphAtlasProvider(&bitmap_glyph_atlas);
        canvas_window->canvas.SetUndoLimit(session_state.undo_limit);

        // Create a new blank canvas with a single base layer.
        canvas_window->canvas.SetColumns(80);
        canvas_window->canvas.EnsureRowsPublic(25);
        canvas_window->canvas.MarkSaved();
        canvas_window->canvas.SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);

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
        canvas_window->canvas.SetBitmapGlyphAtlasProvider(&bitmap_glyph_atlas);
        canvas_window->canvas.SetUndoLimit(session_state.undo_limit);
        canvas_window->canvas.MarkSaved();
        canvas_window->canvas.SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
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
    io_cbs.open_markdown_import_dialog = [&](IoManager::Callbacks::MarkdownPayload&& p)
    {
        markdown_to_ansi_dialog.Open(std::move(p));
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
                             show_brush_palette_window,
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
            io_manager.HandleDialogResult(r, active_canvas, io_cbs, &session_state);
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
            else if (ev.kind == IoManager::OpenEventKind::Palette && !ev.path.empty())
            {
                // Import palette files (currently: GIMP Palette .gpl) into assets/color-palettes.json,
                // then reload the cached palette list so the UI updates immediately.
                std::string err;
                formats::gpl::Palette pal;
                std::string fallback;
                try
                {
                    std::filesystem::path p(ev.path);
                    fallback = p.stem().string();
                }
                catch (...) {}

                if (!formats::gpl::ImportFileToPalette(ev.path, pal, err, fallback))
                {
                    io_manager.SetLastError(err.empty() ? "Failed to import palette." : err);
                    continue;
                }

                ColourPaletteDef def;
                def.title = pal.name.empty() ? fallback : pal.name;
                def.colors.reserve(pal.colors.size());
                for (const auto& c : pal.colors)
                {
                    ImVec4 v;
                    v.x = c.r / 255.0f;
                    v.y = c.g / 255.0f;
                    v.z = c.b / 255.0f;
                    v.w = 1.0f;
                    def.colors.push_back(v);
                }

                const std::string json_path = PhosphorAssetPath("color-palettes.json");
                std::string jerr;
                if (!AppendColourPaletteToJson(json_path.c_str(), std::move(def), jerr))
                {
                    io_manager.SetLastError(jerr.empty() ? "Failed to save palette to color-palettes.json." : jerr);
                    continue;
                }

                // Reload cached list now (not next frame) so open UI refreshes right away.
                std::vector<ColourPaletteDef> prev_palettes = palettes;
                const int prev_selected = xterm_selected_palette;
                std::string reload_err;
                std::vector<ColourPaletteDef> reloaded;
                if (!LoadColourPalettesFromJson(json_path.c_str(), reloaded, reload_err))
                {
                    // Keep prior palettes if reload fails, but surface error.
                    palettes = std::move(prev_palettes);
                    xterm_selected_palette = prev_selected;
                    palettes_loaded = true;
                    io_manager.SetLastError(reload_err.empty() ? "Failed to reload palettes." : reload_err);
                }
                else
                {
                    palettes = std::move(reloaded);
                    palettes_loaded = true;
                    if (!palettes.empty())
                        xterm_selected_palette = (int)palettes.size() - 1;
                }
            }
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
            tool_brush_glyph = (std::uint32_t)phos::glyph::MakeUnicodeScalar((char32_t)cp);
            tool_brush_cp = cp;
            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            if (active_canvas)
                active_canvas->SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
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
                tool_brush_glyph = (std::uint32_t)phos::glyph::MakeUnicodeScalar((char32_t)cp);
                tool_brush_cp = cp;
                // Use the palette's stored UTF-8 string directly (supports multi-codepoint glyphs,
                // and avoids any encode/decode mismatch).
                tool_brush_utf8 = (!utf8.empty()) ? utf8 : ansl::utf8::encode((char32_t)tool_brush_cp);
                if (active_canvas)
                    active_canvas->SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
            }
            else if (g.IsBitmapIndex())
            {
                tool_brush_glyph = (std::uint32_t)phos::glyph::MakeBitmapIndex((std::uint16_t)g.value);
                tool_brush_cp = (std::uint32_t)phos::glyph::ToUnicodeRepresentative((phos::GlyphId)tool_brush_glyph);
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
                if (active_canvas)
                    active_canvas->SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
            }
            else
            {
                // Embedded glyph index: stored as a GlyphId token (lossless).
                tool_brush_glyph = (std::uint32_t)phos::glyph::MakeEmbeddedIndex((std::uint16_t)g.value);
                tool_brush_cp = (std::uint32_t)phos::glyph::ToUnicodeRepresentative((phos::GlyphId)tool_brush_glyph);
                tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
                if (active_canvas)
                    active_canvas->SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
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
            tool_brush_glyph = (std::uint32_t)phos::glyph::MakeUnicodeScalar((char32_t)cp);
            tool_brush_cp = cp;
            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);
            if (active_canvas)
                active_canvas->SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
        }
    }

    namespace fs = std::filesystem;

    // Centralized "insert a glyph at the caret" helper (shared by picker/palette + character sets + hotkeys).
    // Some callers want "typewriter" caret advance; others (Character Sets) want a stationary caret.
    auto insert_glyph_into_canvas = [&](AnsiCanvas* dst, phos::GlyphId glyph, bool advance_caret)
    {
        if (!dst)
            return;
        if (glyph == 0)
            return;

        // Respect current editor FG/BG selection (xterm-256 picker).
        // Canvas uses Color32 where 0 means "unset"; we always apply explicit colours here.
        // Respect current editor FG/BG selection, snapped to the active canvas palette.
        auto to_idx = [&](const ImVec4& c, phos::color::PaletteInstanceId pal) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);
            const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
            return (int)phos::color::ColorOps::NearestIndexRgb(phos::color::GetColorSystem().Palettes(),
                                                               pal,
                                                               (std::uint8_t)std::clamp(r, 0, 255),
                                                               (std::uint8_t)std::clamp(g, 0, 255),
                                                               (std::uint8_t)std::clamp(b, 0, 255),
                                                               qp);
        };
        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        if (dst)
        {
            if (auto id = cs.Palettes().Resolve(dst->GetPaletteRef()))
                pal = *id;
        }
        const AnsiCanvas::ColorIndex16 fg_idx = (AnsiCanvas::ColorIndex16)to_idx(fg_color, pal);
        const AnsiCanvas::ColorIndex16 bg_idx = (AnsiCanvas::ColorIndex16)to_idx(bg_color, pal);

        int caret_x = 0;
        int caret_y = 0;
        dst->GetCaretCell(caret_x, caret_y);

        // Create an undo boundary before mutating so Undo restores the previous state.
        dst->PushUndoSnapshot();

        const int layer_index = dst->GetActiveLayerIndex();
        (void)dst->SetLayerGlyphIndicesPartial(layer_index,
                                              caret_y,
                                              caret_x,
                                              (AnsiCanvas::GlyphId)glyph,
                                              fg_idx,
                                              bg_idx,
                                              std::nullopt);

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

    auto insert_cp_into_canvas = [&](AnsiCanvas* dst, uint32_t cp, bool advance_caret)
    {
        insert_glyph_into_canvas(dst, phos::glyph::MakeUnicodeScalar((char32_t)cp), advance_caret);
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
                insert_glyph_into_canvas(active_canvas, g.ToGlyphId(), /*advance_caret=*/true);
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
        static std::vector<ImVec4> saved_palette_snapped;
        static phos::color::PaletteInstanceId last_snap_palette;

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
        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId snap_pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
        if (active_canvas)
        {
            if (auto id = cs.Palettes().Resolve(active_canvas->GetPaletteRef()))
                snap_pal = *id;
        }
        if (xterm_picker_mode == 0)
            value_changed = ImGui::ColorPicker4_Xterm256_HueBar("##picker", picker_col, false,
                                                               &used_right, &xterm_picker_last_hue,
                                                               saved_palette_snapped.data(), (int)saved_palette_snapped.size(),
                                                               snap_pal);
        else
            value_changed = ImGui::ColorPicker4_Xterm256_HueWheel("##picker", picker_col, false,
                                                                 &used_right, &xterm_picker_last_hue,
                                                                 saved_palette_snapped.data(), (int)saved_palette_snapped.size(),
                                                                 snap_pal);

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

        // Apply/convert the canvas palette to match the selected UI palette.
        // This is an explicit operation (not done automatically when browsing palettes),
        // because it remaps the entire document and changes the canvas palette index space.
        if (active_canvas)
        {
            const bool can_apply = (xterm_selected_palette >= 0 && xterm_selected_palette < (int)palettes.size());
            if (!can_apply)
                ImGui::BeginDisabled();
            if (ImGui::Button("Set Canvas Palette"))
            {
                const ColourPaletteDef& sel = palettes[xterm_selected_palette];
                std::vector<phos::color::Rgb8> rgb;
                rgb.reserve(std::min<std::size_t>(sel.colors.size(), phos::color::kMaxPaletteSize));
                for (std::size_t i = 0; i < sel.colors.size() && rgb.size() < phos::color::kMaxPaletteSize; ++i)
                {
                    const ImVec4& c = sel.colors[i];
                    const int r = (int)std::lround(c.x * 255.0f);
                    const int g = (int)std::lround(c.y * 255.0f);
                    const int b = (int)std::lround(c.z * 255.0f);
                    rgb.push_back(phos::color::Rgb8{
                        (std::uint8_t)std::clamp(r, 0, 255),
                        (std::uint8_t)std::clamp(g, 0, 255),
                        (std::uint8_t)std::clamp(b, 0, 255),
                    });
                }

                if (!rgb.empty())
                {
                    auto& cs2 = phos::color::GetColorSystem();
                    const phos::color::PaletteInstanceId pid = cs2.Palettes().RegisterDynamic(sel.title, rgb);
                    if (const phos::color::Palette* pnew = cs2.Palettes().Get(pid))
                    {
                        active_canvas->SetColourPaletteTitle(sel.title);
                        (void)active_canvas->ConvertToPalette(pnew->ref);
                        // Force the picker to rebuild its snapped palette against the new active palette.
                        last_snap_palette = phos::color::PaletteInstanceId{};
                        last_palette_index = -1;
                    }
                }
            }
            if (!can_apply)
                ImGui::EndDisabled();
        }

        // Rebuild working palette when selection changes.
        const bool need_rebuild_palette =
            (xterm_selected_palette != last_palette_index) ||
            (snap_pal != last_snap_palette);
        if (need_rebuild_palette && !palettes.empty())
        {
            saved_palette = palettes[xterm_selected_palette].colors;
            last_palette_index = xterm_selected_palette;
            if (active_canvas && xterm_selected_palette >= 0 && xterm_selected_palette < (int)palettes.size())
                active_canvas->SetColourPaletteTitle(palettes[xterm_selected_palette].title);

            // Build a snapped version of the selected UI palette against the *active canvas palette*.
            //
            // IMPORTANT:
            // - We keep `saved_palette` as the raw UI palette colors so browsing palettes is stable
            //   (swatches should not visually change when the active canvas palette changes).
            // - We use `saved_palette_snapped` only as a mapping helper for picking/quantized selection.
            saved_palette_snapped.clear();
            saved_palette_snapped.reserve(saved_palette.size());
            const phos::color::Palette* sp = cs.Palettes().Get(snap_pal);
            const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
            for (const ImVec4& c : saved_palette)
            {
                if (!sp || sp->rgb.empty())
                {
                    saved_palette_snapped.push_back(c);
                    continue;
                }
                const int r = (int)std::lround(c.x * 255.0f);
                const int g = (int)std::lround(c.y * 255.0f);
                const int b = (int)std::lround(c.z * 255.0f);
                const std::uint8_t idx = phos::color::ColorOps::NearestIndexRgb(cs.Palettes(),
                                                                                snap_pal,
                                                                                (std::uint8_t)std::clamp(r, 0, 255),
                                                                                (std::uint8_t)std::clamp(g, 0, 255),
                                                                                (std::uint8_t)std::clamp(b, 0, 255),
                                                                                qp);
                if ((std::size_t)idx >= sp->rgb.size())
                {
                    saved_palette_snapped.push_back(c);
                    continue;
                }
                const phos::color::Rgb8 prgb = sp->rgb[(size_t)idx];
                saved_palette_snapped.push_back(ImVec4(prgb.r / 255.0f, prgb.g / 255.0f, prgb.b / 255.0f, c.w));
            }
            last_snap_palette = snap_pal;
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
        
        auto same_rgb = [](const ImVec4& a, const ImVec4& b) -> bool {
            // Colors are normalized floats; compare with ~0.5/255 tolerance.
            const float eps = 0.002f;
            return (std::fabs(a.x - b.x) <= eps) &&
                   (std::fabs(a.y - b.y) <= eps) &&
                   (std::fabs(a.z - b.z) <= eps);
        };

        for (int n = 0; n < count; n++)
        {
            ImGui::PushID(n);
            if (n % cols != 0)
                ImGui::SameLine(0.0f, style.ItemSpacing.x);

            // Mark selection based on the *effective* snapped color (palette entry), even though
            // we display the raw UI palette swatch.
            const ImVec4 snapped = (n < (int)saved_palette_snapped.size()) ? saved_palette_snapped[n] : saved_palette[n];
            const bool mark_fg = same_rgb(snapped, fg_color);
            const bool mark_bg = same_rgb(snapped, bg_color);
            const ColourPaletteSwatchAction a =
                RenderColourPaletteSwatchButton("##palette", saved_palette[n], button_size, mark_fg, mark_bg);
            if (a.set_primary)
            {
                // Set the editor FG/BG to the snapped palette entry so downstream code
                // (tools, ANSL, etc.) operates in the active palette index space.
                pal_primary.x = snapped.x;
                pal_primary.y = snapped.y;
                pal_primary.z = snapped.z;
            }
            if (a.set_secondary)
            {
                pal_secondary.x = snapped.x;
                pal_secondary.y = snapped.y;
                pal_secondary.z = snapped.z;
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

            // Keep keybinding engine's tool action registry in sync with the current tool set
            // (used by Settings UI and the host action router).
            std::vector<kb::Action> all;
            for (const ToolSpec& t : tool_palette.GetTools())
                for (const kb::Action& a : t.actions)
                    all.push_back(a);
            keybinds.SetToolActions(std::move(all));
        }

        std::string tool_path;
        if (tool_palette.TakeActiveToolChanged(tool_path))
        {
            // Persist params of the previously-compiled tool before compiling the new script.
            tool_params::SaveToolParamsToSession(session_state, s_compiled_tool_id, tool_engine);

            if (st.tools.compile_tool_script) st.tools.compile_tool_script(tool_path);
            if (st.tools.sync_tool_stack) st.tools.sync_tool_stack();

            // If compilation succeeded, restore saved params for the newly-active tool.
            if (tool_compile_error.empty())
            {
                s_compiled_tool_id = active_tool_id();
                tool_params::RestoreToolParamsFromSession(session_state, s_compiled_tool_id, tool_engine);
                if (const ToolSpec* t = tool_palette.GetActiveTool())
                    session_state.active_tool_path = t->path;
            }
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

        // Tool parameters UI (settings.params -> ctx.params), extracted into src/ui.
        static ToolParametersWindow tool_params_window;
        (void)tool_params_window.Render(tool_palette.GetActiveTool(),
                                        s_compiled_tool_id,
                                        tool_engine,
                                        session_state,
                                        should_apply_placement("Tool Parameters"));
    }

    // Render each canvas window
    for (size_t i = 0; i < canvases.size(); ++i)
    {
        if (!canvases[i])
            continue;
        CanvasWindow& canvas = *canvases[i];
        // Ensure atlas provider is attached for restored canvases (session restore happens in main()).
        if (canvas.canvas.GetBitmapGlyphAtlasProvider() != &bitmap_glyph_atlas)
            canvas.canvas.SetBitmapGlyphAtlasProvider(&bitmap_glyph_atlas);
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

        // Canvas window identity:
        // - We want the visible title to change (e.g. dirty "* " prefix),
        //   but we must keep the ImGui window ID stable to avoid one-frame "jumps"
        //   (ImGui will treat a renamed window as a different window and may re-apply defaults).
        // - Use the "###" separator so only the suffix participates in the window ID.
        //
        // Persistence strategy:
        // - Keep the *window* ID per-instance (includes canvas.id) so multiple windows can share a file path.
        // - Keep the *placement* key stable per-document for file-backed canvases, to prevent session.json growth
        //   when the same file is opened repeatedly (canvas ids keep increasing).
        const std::string doc_id = sanitize_imgui_id(canvas_path);
        const std::string canvas_window_id = "canvas:" + doc_id + "#" + std::to_string(canvas.id);
        const std::string session_canvas_dir = PhosphorCachePath("session_canvases");
        const bool is_session_canvas =
            (!session_canvas_dir.empty() && canvas_path.rfind(session_canvas_dir, 0) == 0);
        const std::string persist_key = (!is_session_canvas ? ("canvas:" + doc_id) : canvas_window_id);
        const bool dirty = canvas.canvas.IsModifiedSinceLastSave();
        std::string title = (dirty ? "* " : "") + canvas_path + "###" + canvas_window_id;

        const auto it = session_state.imgui_windows.find(persist_key);
        const bool has_saved = (it != session_state.imgui_windows.end() && it->second.valid);

        // First-time placement sizing block is unchanged from main.cpp.
        if (!has_saved)
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 work_pos  = vp ? vp->WorkPos  : ImVec2(0, 0);
            const ImVec2 work_size = vp ? vp->WorkSize : ImVec2(1280, 720);
            const ImVec2 center(work_pos.x + work_size.x * 0.5f,
                                work_pos.y + work_size.y * 0.5f);

            // IMPORTANT: initial canvas window sizing must match the canvas renderer's cell metrics.
            // Some canvas fonts are 8x16, others are 9x16, 8x8, or embedded bitmap fonts; using a
            // fixed ratio (e.g. font_size * 0.5f) causes incorrect initial width for many fonts.
            ImFont* font = ImGui::GetFont();
            const float base_font_size = ImGui::GetFontSize();
            const fonts::FontInfo& finfo = fonts::Get(canvas.canvas.GetFontId());
            const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.canvas.GetEmbeddedFont();
            const bool embedded_font =
                (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
                 ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);

            float base_cell_w = 0.0f;
            float base_cell_h = 0.0f;
            if (embedded_font)
            {
                // Keep initial sizing consistent with AnsiCanvas::Render():
                // bitmap/embedded fonts use native pixel metrics (not scaled by UI font size).
                base_cell_w = (float)ef->cell_w;
                base_cell_h = (float)ef->cell_h;
            }
            else if (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0)
            {
                base_cell_w = (float)finfo.cell_w;
                base_cell_h = (float)finfo.cell_h;
            }
            else
            {
                // ImGui atlas font case (e.g. Unscii): sample a representative glyph width.
                base_cell_w = font ? font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x : 0.0f;
                base_cell_h = base_font_size;
            }

            base_cell_w = std::max(1.0f, base_cell_w);
            base_cell_h = std::max(1.0f, base_cell_h);
            const float zoom = canvas.canvas.GetZoom();

            const float snapped_scale = canvas.canvas.SnappedScaleForZoom(zoom, base_cell_w);
            float scaled_cell_w = std::floor(base_cell_w * snapped_scale + 0.5f);
            float scaled_cell_h = std::floor(base_cell_h * snapped_scale + 0.5f);
            if (scaled_cell_w < 1.0f) scaled_cell_w = 1.0f;
            if (scaled_cell_h < 1.0f) scaled_cell_h = 1.0f;

            const int cols2 = canvas.canvas.GetColumns();
            const int rows2 = canvas.canvas.GetRows();
            // Add +1 cell of horizontal slack so the initial window doesn't start with a tiny
            // horizontal scrollbar due to rounding/inner-rect thresholds.
            const ImVec2 grid_px(scaled_cell_w * (float)(cols2 + 2),
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

            // NOTE: Avoid pivot-based centering here.
            // For newly-created windows, ImGui's size isn't always fully "settled" on the very first
            // Begin(), and pivot-centering can cause a visible one-frame jump.
            const float offset = 18.0f * (float)((canvas.id - 1) % 10);
            const ImVec2 centered(center.x + offset, center.y + offset);
            const ImVec2 top_left(centered.x - desired.x * 0.5f,
                                  centered.y - desired.y * 0.5f);
            ImGui::SetNextWindowPos(top_left, ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(desired, ImGuiCond_Appearing);
        }

        ApplyImGuiWindowPlacement(session_state, persist_key.c_str(),
                                  has_saved && should_apply_placement(persist_key.c_str()));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_None | GetImGuiWindowChromeExtraFlags(session_state, title.c_str());
        const bool alpha_pushed = PushImGuiWindowChromeAlpha(&session_state, title.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool open_before_begin = canvas.open;
        ImGui::Begin(title.c_str(), &canvas.open, flags);
        CaptureImGuiWindowPlacement(session_state, persist_key.c_str());
        ApplyImGuiWindowChromeZOrder(&session_state, title.c_str());
        RenderImGuiWindowChromeMenu(&session_state, title.c_str());

        // Title-bar ⛶ button: Reset Zoom (1:1).
        {
            ImVec2 rect_min(0.0f, 0.0f), rect_max(0.0f, 0.0f);
            const bool has_close = true; // canvas windows always have a close button
            const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
            if (RenderImGuiWindowChromeTitleBarButton("##canvas_reset_zoom", "\xE2\x9B\xB6", has_close, has_collapse,
                                                      &rect_min, &rect_max))
            {
                const AnsiCanvas::ViewState vs = canvas.canvas.GetLastViewState();
                if (vs.valid && vs.canvas_w > 0.0f && vs.canvas_h > 0.0f)
                {
                    const float old_zoom = canvas.canvas.GetZoom();
                    const float base_cell_w = (vs.base_cell_w > 0.0f) ? vs.base_cell_w : 8.0f;
                    const float old_scale = canvas.canvas.SnappedScaleForZoom(old_zoom, base_cell_w);
                    const float focus_x = vs.scroll_x + vs.view_w * 0.5f;
                    const float focus_y = vs.scroll_y + vs.view_h * 0.5f;

                    canvas.canvas.SetZoom(1.0f);
                    const float new_scale = canvas.canvas.SnappedScaleForZoom(canvas.canvas.GetZoom(), base_cell_w);
                    const float ratio = (old_scale > 0.0f) ? (new_scale / old_scale) : 1.0f;
                    canvas.canvas.RequestScrollPixels(focus_x * ratio - vs.view_w * 0.5f,
                                                      focus_y * ratio - vs.view_h * 0.5f);
                }
                else
                {
                    canvas.canvas.SetZoom(1.0f);
                }
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Reset Zoom (1:1)");
                ImGui::EndTooltip();
            }
        }

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

        // Current FG/BG selection quantized to the active canvas palette.
        auto& cs = phos::color::GetColorSystem();
        phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);

        auto tool_runner = [&](AnsiCanvas& c, int phase) {
            if (!tool_engine.HasRenderFunction())
                return;

            if (auto id = cs.Palettes().Resolve(c.GetPaletteRef()))
                pal = *id;

            AnslFrameContext ctx;
            std::vector<int> allowed_indices;
            std::vector<std::uint32_t> glyph_candidates;
            std::vector<phos::GlyphId> glyph_id_candidates;
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
            {
                auto to_idx_pal = [&](const ImVec4& col) -> int {
                    const int r = (int)std::lround(col.x * 255.0f);
                    const int g = (int)std::lround(col.y * 255.0f);
                    const int b = (int)std::lround(col.z * 255.0f);
                    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
                    return (int)phos::color::ColorOps::NearestIndexRgb(cs.Palettes(), pal,
                                                                       (std::uint8_t)std::clamp(r, 0, 255),
                                                                       (std::uint8_t)std::clamp(g, 0, 255),
                                                                       (std::uint8_t)std::clamp(b, 0, 255),
                                                                       qp);
                };
                ctx.fg = to_idx_pal(fg_color);
                ctx.bg = to_idx_pal(bg_color);
            }
            ctx.palette_is_builtin = c.GetPaletteRef().is_builtin;
            ctx.palette_builtin = (std::uint32_t)c.GetPaletteRef().builtin;
            ctx.glyph_utf8 = tool_brush_utf8;
            ctx.glyph_cp = (int)tool_brush_cp;
            ctx.glyph_id = (std::uint32_t)tool_brush_glyph;
            ctx.attrs = tool_attrs_mask;
            ctx.allowed_indices = nullptr;
            ctx.glyph_candidates = nullptr;
            ctx.glyph_id_candidates = nullptr;
            ctx.allow_caret_writeback = true;
            // Multi-cell brush stamp (optional; provided by the canvas).
            AnslFrameContext::BrushStamp stamp;
            ctx.brush = nullptr;
            if (const AnsiCanvas::Brush* b = c.GetCurrentBrush())
            {
                stamp.w = b->w;
                stamp.h = b->h;
                stamp.glyph = (const std::uint32_t*)b->cp.data();
                stamp.cp = nullptr; // legacy (best-effort); scripts should prefer cell.glyph
                // Index-native (Phase B): expose indices directly in the canvas palette space.
                stamp.fg = b->fg.data();
                stamp.bg = b->bg.data();
                stamp.attrs = b->attrs.data();
                ctx.brush = &stamp;
            }

            c.GetCaretCell(ctx.caret_x, ctx.caret_y);

        // Active palette: expose allowed indices to tools (for quantization/snapping).
        // These indices are in the canvas's active palette index space (canvas.palette_ref).
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

            auto& cs = phos::color::GetColorSystem();
            phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
            if (auto id = cs.Palettes().Resolve(c.GetPaletteRef()))
                pal = *id;
            std::unordered_set<int> seen;
                seen.reserve(def->colors.size());
                for (const ImVec4& ccol : def->colors)
                {
                    const int r = (int)std::lround(ccol.x * 255.0f);
                    const int g = (int)std::lround(ccol.y * 255.0f);
                    const int b = (int)std::lround(ccol.z * 255.0f);
                const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
                const int idx = (int)phos::color::ColorOps::NearestIndexRgb(cs.Palettes(), pal,
                                                                            (std::uint8_t)std::clamp(r, 0, 255),
                                                                            (std::uint8_t)std::clamp(g, 0, 255),
                                                                            (std::uint8_t)std::clamp(b, 0, 255),
                                                                            qp);
                    if (seen.insert(idx).second)
                        allowed_indices.push_back(idx);
                }
                if (!allowed_indices.empty())
                    ctx.allowed_indices = &allowed_indices;
            }

            // Candidate glyph set: limit expensive glyph-search tools (e.g. deform quantization)
            // to the Character Palette + whatever glyphs already exist on the canvas.
            // The canvas-glyph portion is collected by the native tool, but the UI palette portion
            // must come from the host (CharacterPalette).
            character_palette.CollectCandidateCodepoints(glyph_candidates, &c);
            if (!glyph_candidates.empty())
                ctx.glyph_candidates = &glyph_candidates;

            character_palette.CollectCandidateGlyphIds(glyph_id_candidates, &c);
            if (!glyph_id_candidates.empty())
                ctx.glyph_id_candidates = &glyph_id_candidates;

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

                // -----------------------
                // Action Router (Option A)
                // -----------------------
                // Precedence: active tool > fallback tool handlers > host.
                //
                // We start by evaluating a small "common action layer" through the key bindings engine,
                // then route based on explicit tool handles (settings.handles).
                //
                // This makes actions like selection delete work even when Select isn't the active tool,
                // while still letting tools (e.g. Edit) override behavior.

                const ToolSpec* active_tool = tool_palette.GetActiveTool();

                // Cache fallback tool engines across frames (path -> engine).
                static std::unordered_map<std::string, FallbackToolState> fallback_tools;

                auto ensure_fallback_engine = [&](const ToolSpec& t) -> AnslScriptEngine* {
                    if (t.path.empty())
                        return nullptr;
                    FallbackToolState& st = fallback_tools[t.path];
                    if (!st.engine)
                    {
                        st.engine = std::make_unique<AnslScriptEngine>();
                        std::string err;
                        if (!st.engine->Init(GetPhosphorAssetsDir(), err, &session_state.font_sanity_cache, false))
                        {
                            st.last_error = err;
                            st.engine.reset();
                            return nullptr;
                        }
                    }

                    const std::string src = ReadFileToString(t.path);
                    if (src.empty())
                        return st.engine.get();
                    if (src != st.last_source)
                    {
                        std::string err;
                        // Compile with the active canvas so palette-aware helpers (ansl.color.*)
                        // produce indices in the correct palette at load time.
                        if (!st.engine->CompileUserScript(src, active_canvas, err))
                        {
                            st.last_error = err;
                            return st.engine.get();
                        }
                        st.last_error.clear();
                        st.last_source = src;
                    }
                    return st.engine.get();
                };

                auto run_fallback_tool_action = [&](const ToolSpec& t, std::string_view action_id) -> bool {
                    AnslScriptEngine* eng = ensure_fallback_engine(t);
                    if (!eng)
                        return false;

                    std::vector<std::string> actions;
                    actions.emplace_back(action_id);

                    AnslFrameContext fctx = ctx;
                    // Keyboard-only dispatch.
                    fctx.phase = 0;
                    // Avoid accidental key-driven behavior in the fallback tool: drive only via ctx.actions.
                    fctx.key_left = false;
                    fctx.key_right = false;
                    fctx.key_up = false;
                    fctx.key_down = false;
                    fctx.key_home = false;
                    fctx.key_end = false;
                    fctx.key_backspace = false;
                    fctx.key_delete = false;
                    fctx.key_enter = false;
                    fctx.key_c = false;
                    fctx.key_v = false;
                    fctx.key_x = false;
                    fctx.key_a = false;
                    fctx.key_escape = false;
                    fctx.hotkeys = {};
                    fctx.typed = nullptr;
                    fctx.cursor_valid = false;
                    fctx.actions_pressed = &actions;
                    fctx.allow_caret_writeback = false;

                    ToolCommandSink sink;
                    sink.allow_tool_commands = false;
                    sink.out_commands = nullptr;

                    std::string err;
                    const bool ok = eng->RunFrame(c, c.GetActiveLayerIndex(), fctx, sink, false, err);
                    (void)ok;
                    // Even if the tool errors, don't crash routing; treat as handled to avoid host fallback duplication.
                    return true;
                };

                auto host_fallback = [&](std::string_view action_id) -> bool {
                    if (action_id == "edit.select_all")
                    {
                        c.SelectAll();
                        return true;
                    }
                    if (action_id == "selection.clear_or_cancel")
                    {
                        if (c.IsMovingSelection())
                            (void)c.CancelMoveSelection();
                        else
                            c.ClearSelection();
                        return true;
                    }
                    if (action_id == "selection.delete")
                    {
                        if (c.IsMovingSelection())
                            (void)c.CommitMoveSelection();
                        (void)c.DeleteSelection();
                        return true;
                    }
                    if (action_id == "edit.copy")
                    {
                        (void)app::CopySelectionToSystemClipboardText(c);
                        return c.CopySelectionToClipboard();
                    }
                    if (action_id == "edit.cut")
                    {
                        (void)app::CopySelectionToSystemClipboardText(c);
                        return c.CutSelectionToClipboard();
                    }
                    if (action_id == "edit.paste")
                    {
                        if (app::PasteSystemClipboardText(c, ctx.caret_x, ctx.caret_y))
                            return true;
                        return c.PasteClipboard(ctx.caret_x, ctx.caret_y);
                    }
                    return false;
                };

                // Evaluate common semantic hotkeys from the keybinding engine.
                const kb::Hotkeys hk_raw = keybinds.EvalCommonHotkeys(kctx);
                struct Candidate
                {
                    std::string_view id;
                    bool             pressed = false;
                };
                Candidate candidates[] = {
                    {"edit.copy", hk_raw.copy},
                    {"edit.cut", hk_raw.cut},
                    {"edit.paste", hk_raw.paste},
                    {"edit.select_all", hk_raw.select_all},
                    {"selection.clear_or_cancel", hk_raw.cancel},
                    {"selection.delete", hk_raw.delete_selection},
                };

                // Decide which of the common actions to deliver to the active tool, and which to handle via fallback.
                // - If the active tool handles the action (when="active"): deliver it via ctx.hotkeys + ctx.actions.
                // - Otherwise: run the first fallback tool that handles it (when="inactive"), excluding the active tool.
                // - Otherwise: host fallback.
                kb::Hotkeys hk_to_tool;
                pressed_actions.clear();
                bool request_switch_to_select_tool = false;

                for (const Candidate& cand : candidates)
                {
                    if (!cand.pressed)
                        continue;

                    const bool claimed_by_active = ToolClaimsAction(active_tool, cand.id);
                    if (claimed_by_active)
                    {
                        // If a tool claims clipboard actions, we still want OS clipboard interop:
                        // - Copy/Cut: mirror selection to OS clipboard as UTF-8 text.
                        // - Paste: prefer OS clipboard paste; if it succeeds, don't also deliver to the tool
                        //          (to avoid double-paste). If it fails, fall back to tool behavior.
                        if (cand.id == "edit.copy" || cand.id == "edit.cut")
                        {
                            (void)app::CopySelectionToSystemClipboardText(c);
                        }
                        else if (cand.id == "edit.paste")
                        {
                            if (app::PasteSystemClipboardText(c, ctx.caret_x, ctx.caret_y))
                            {
                                // After pasting, switch to Select so the pasted region can be moved immediately.
                                request_switch_to_select_tool = true;
                                continue;
                            }
                        }

                        pressed_actions.push_back(std::string(cand.id));
                        if (cand.id == "edit.copy") hk_to_tool.copy = true;
                        else if (cand.id == "edit.cut") hk_to_tool.cut = true;
                        else if (cand.id == "edit.paste") hk_to_tool.paste = true;
                        else if (cand.id == "edit.select_all") hk_to_tool.select_all = true;
                        else if (cand.id == "selection.clear_or_cancel") hk_to_tool.cancel = true;
                        else if (cand.id == "selection.delete") hk_to_tool.delete_selection = true;
                        continue;
                    }

                    bool handled = false;
                    for (const ToolSpec& t : tool_palette.GetTools())
                    {
                        if (active_tool && t.id == active_tool->id)
                            continue;
                        if (!ToolFallbackClaimsAction(t, cand.id))
                            continue;
                        handled = run_fallback_tool_action(t, cand.id);
                        if (handled)
                            break;
                    }
                    if (!handled)
                        (void)host_fallback(cand.id);
                }

                // Expose routed hotkeys/actions to the active tool.
                ctx.hotkeys.copy = hk_to_tool.copy;
                ctx.hotkeys.cut = hk_to_tool.cut;
                ctx.hotkeys.paste = hk_to_tool.paste;
                ctx.hotkeys.selectAll = hk_to_tool.select_all;
                ctx.hotkeys.cancel = hk_to_tool.cancel;
                ctx.hotkeys.deleteSelection = hk_to_tool.delete_selection;

                if (request_switch_to_select_tool)
                {
                    (void)tool_palette.SetActiveToolById("select");
                }

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
                    const phos::color::Palette* p = cs.Palettes().Get(pal);
                    if (!p || p->rgb.empty())
                        return;
                    idx = std::clamp(idx, 0, (int)p->rgb.size() - 1);
                    const phos::color::Rgb8 rgb = p->rgb[(size_t)idx];
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
                            const uint32_t v = cmd.brush_cp;
                            // Tool commands can pass either:
                            // - a Unicode scalar (<= 0x10FFFF), or
                            // - a GlyphId token (>= 0x80000000).
                            if (v >= 0x80000000u)
                                tool_brush_glyph = v;
                            else
                                tool_brush_glyph = (std::uint32_t)phos::glyph::MakeUnicodeScalar((char32_t)v);

                            tool_brush_cp = (std::uint32_t)phos::glyph::ToUnicodeRepresentative((phos::GlyphId)tool_brush_glyph);
                            tool_brush_utf8 = ansl::utf8::encode((char32_t)tool_brush_cp);

                            // Only synchronize Unicode-focused UI widgets when this is a Unicode scalar.
                            if (v < 0x80000000u)
                            {
                                character_picker.JumpToCodePoint(v);
                                character_palette.OnPickerSelectedCodePoint(v);
                                character_sets.OnExternalSelectedCodePoint(v);
                            }

                            c.SetActiveGlyph((phos::GlyphId)tool_brush_glyph, tool_brush_utf8);
                        }
                    } break;
                    case ToolCommand::Type::PaletteSet:
                    {
                        if (cmd.has_fg)
                            apply_idx_to_color(cmd.fg, fg_color);
                        if (cmd.has_bg)
                            apply_idx_to_color(cmd.bg, bg_color);
                    } break;
                    case ToolCommand::Type::AttrsSet:
                    {
                        tool_attrs_mask = cmd.attrs;
                    } break;
                    case ToolCommand::Type::ToolActivatePrev:
                    {
                        tool_params::SaveToolParamsToSession(session_state, s_compiled_tool_id, tool_engine);
                        activate_prev_tool();
                        if (tool_compile_error.empty())
                        {
                            s_compiled_tool_id = active_tool_id();
                            tool_params::RestoreToolParamsFromSession(session_state, s_compiled_tool_id, tool_engine);
                            if (const ToolSpec* t = tool_palette.GetActiveTool())
                                session_state.active_tool_path = t->path;
                        }
                    } break;
                    case ToolCommand::Type::ToolActivate:
                    {
                        tool_params::SaveToolParamsToSession(session_state, s_compiled_tool_id, tool_engine);
                        activate_tool_by_id(cmd.tool_id);
                        if (tool_compile_error.empty())
                        {
                            s_compiled_tool_id = active_tool_id();
                            tool_params::RestoreToolParamsFromSession(session_state, s_compiled_tool_id, tool_engine);
                            if (const ToolSpec* t = tool_palette.GetActiveTool())
                                session_state.active_tool_path = t->path;
                        }
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
                            AnsiCanvas::ColorIndex16 fg = AnsiCanvas::kUnsetIndex16;
                            AnsiCanvas::ColorIndex16 bg = AnsiCanvas::kUnsetIndex16;
                            AnsiCanvas::Attrs attrs = 0;
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
                                    (void)c.GetLayerCellIndices(li, sy, sx, cell.fg, cell.bg);
                                    (void)c.GetLayerCellAttrs(li, sy, sx, cell.attrs);
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
                                    (void)c.SetLayerCellIndices(li, y, x, cell.cp, cell.fg, cell.bg, cell.attrs);
                                }
                        }

                        c.SetSelectionCorners(0, 0, r.w - 1, r.h - 1);
                    } break;
                    case ToolCommand::Type::BrushPreviewSet:
                    {
                        // Transient: the canvas clears this each frame; tools should re-send while active.
                        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

                        if (cmd.preview_has_rect)
                        {
                            x0 = cmd.preview_x0;
                            y0 = cmd.preview_y0;
                            x1 = cmd.preview_x1;
                            y1 = cmd.preview_y1;
                        }
                        else
                        {
                            int ax = 0, ay = 0;
                            bool anchor_ok = true;
                            if (cmd.preview_anchor == ToolCommand::BrushPreviewAnchor::Caret)
                            {
                                ax = ctx.caret_x;
                                ay = ctx.caret_y;
                            }
                            else
                            {
                                // Cursor-anchored previews should only show when the cursor is valid.
                                if (!ctx.cursor_valid)
                                    anchor_ok = false;
                                ax = ctx.cursor_x;
                                ay = ctx.cursor_y;
                            }

                            if (anchor_ok)
                            {
                                const int rx = std::max(0, cmd.preview_rx);
                                const int ry = std::max(0, cmd.preview_ry);
                                const int ox = cmd.preview_ox;
                                const int oy = cmd.preview_oy;
                                x0 = (ax + ox) - rx;
                                y0 = (ay + oy) - ry;
                                x1 = (ax + ox) + rx;
                                y1 = (ay + oy) + ry;
                            }
                        }

                        if (x1 >= x0 && y1 >= y0)
                            c.SetToolBrushPreviewRect(x0, y0, x1, y1);
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

    // Brush Palette window
    if (show_brush_palette_window)
    {
        const char* name = "Brush Palette";
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        brush_palette.Render(name, &show_brush_palette_window, ui_active_canvas,
                             &session_state, should_apply_placement(name));

        // UX: selecting/creating a brush implies "I want to stamp now", so auto-switch
        // to the Brush tool unless it's already active.
        if (brush_palette.TakeActivateBrushToolRequested())
        {
            if (active_tool_id() != "02-brush")
                activate_tool_by_id("02-brush");
        }
    }

    // Layer Manager window
    if (show_layer_manager_window)
    {
        const char* name = "Layer Manager";
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        layer_manager.Render(name, &show_layer_manager_window, ui_active_canvas,
                             &session_state, should_apply_placement(name), layer_thumbnails_refresh_release);
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
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);

        // ANSL contract: ctx.fg/ctx.bg are indices in the *active canvas palette* (not xterm indices).
        auto to_idx = [&](const ImVec4& c) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);

            auto& cs = phos::color::GetColorSystem();
            phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
            if (ui_active_canvas)
            {
                if (auto id = cs.Palettes().Resolve(ui_active_canvas->GetPaletteRef()))
                    pal = *id;
            }

            const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
            return (int)phos::color::ColorOps::NearestIndexRgb(cs.Palettes(),
                                                               pal,
                                                               (std::uint8_t)std::clamp(r, 0, 255),
                                                               (std::uint8_t)std::clamp(g, 0, 255),
                                                               (std::uint8_t)std::clamp(b, 0, 255),
                                                               qp);
        };
        const int fg_idx2 = to_idx(fg_color);
        const int bg_idx2 = to_idx(bg_color);
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
        const std::string persist_key = "image:" + img_path;

        RenderImageWindow(title.c_str(), persist_key.c_str(), img, image_to_chafa_dialog,
                          &session_state, should_apply_placement(persist_key.c_str()));
    }

    // Minimap window
    if (show_minimap_window)
    {
        const char* name = "Minimap";
        // Important: resolve *UI active* canvas late (after canvas windows updated last_active_canvas_id)
        // so the minimap tracks the currently focused canvas window.
        AnsiCanvas* ui_active_canvas = ResolveUiActiveCanvas(canvases, last_active_canvas_id);
        preview_texture.Update(ui_active_canvas, 768, ImGui::GetTime());
        const CanvasPreviewTextureView pv_view = preview_texture.View();
        minimap_window.Render(name, &show_minimap_window, ui_active_canvas, &pv_view,
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
        settings_window.SetLutCacheBudgetApplier([&](size_t bytes) {
            // LUT cache is not yet implemented as a core service; persist the preference now.
            // Apply immediately (LutCache is global in ColorSystem for now).
            session_state.lut_cache_budget_bytes = bytes;
            phos::color::GetColorSystem().Luts().SetBudgetBytes(bytes);
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

    // Markdown import UI
    markdown_to_ansi_dialog.Render(&session_state,
                                   should_apply_placement("Markdown \xE2\x86\x92 Canvas##md_preview"));
    {
        const std::string src_path = markdown_to_ansi_dialog.SourcePath();
        AnsiCanvas imported;
        if (markdown_to_ansi_dialog.TakeAccepted(imported))
        {
            // Mark the canvas as "imported from markdown" without making Save overwrite the source file.
            if (!src_path.empty())
                imported.SetFilePath(std::string("md://") + src_path);

            auto canvas_window = std::make_unique<CanvasWindow>();
            canvas_window->open = true;
            canvas_window->id = next_canvas_id++;
            canvas_window->canvas = std::move(imported);
            canvas_window->canvas.SetKeyBindingsEngine(&keybinds);
            canvas_window->canvas.SetUndoLimit(session_state.undo_limit);
            canvas_window->canvas.MarkSaved();
            last_active_canvas_id = canvas_window->id;
            canvases.push_back(std::move(canvas_window));

            // Update Recent with the original markdown source path (not the md:// pseudo-path).
            push_recent(src_path);
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
                                                         show_brush_palette_window,
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


