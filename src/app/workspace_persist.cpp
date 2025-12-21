#include "app/workspace_persist.h"

#include <algorithm>
#include <cstdio>

#include "ansl/ansl_native.h"
#include "io/image_loader.h"
#include "io/session/open_canvas_cache.h"
#include "io/session/open_canvas_codec.h"

namespace workspace_persist
{

void RestoreWorkspaceFromSession(const SessionState& session_state,
                                 kb::KeyBindingsEngine& keybinds,
                                 std::vector<std::unique_ptr<CanvasWindow>>& canvases,
                                 int& next_canvas_id,
                                 int& last_active_canvas_id,
                                 std::vector<ImageWindow>& images,
                                 int& next_image_id)
{
    // Restore workspace content (open canvases + images).
    if (session_state.next_canvas_id > 0)
        next_canvas_id = session_state.next_canvas_id;
    if (session_state.next_image_id > 0)
        next_image_id = session_state.next_image_id;
    last_active_canvas_id = session_state.last_active_canvas_id;

    // Restore canvases.
    for (const auto& oc : session_state.open_canvases)
    {
        auto cw = std::make_unique<CanvasWindow>();
        cw->open = oc.open;
        cw->id = (oc.id > 0) ? oc.id : next_canvas_id++;
        cw->canvas.SetKeyBindingsEngine(&keybinds);
        if (!oc.file_path.empty())
            cw->canvas.SetFilePath(oc.file_path);
        // Loaded/restored canvases are "clean" until the user edits.
        cw->canvas.MarkSaved();

        // Per-canvas active glyph selection (tools brush).
        {
            std::uint32_t cp = oc.active_glyph_cp;
            if (cp == 0)
                cp = (std::uint32_t)U' ';
            std::string utf8 = oc.active_glyph_utf8;
            if (utf8.empty())
                utf8 = ansl::utf8::encode((char32_t)cp);
            cw->canvas.SetActiveGlyph(cp, std::move(utf8));
        }

        // Prefer cache-backed restore (fast session.json parse, project loaded lazily).
        if (!oc.project_phos_cache_rel.empty())
        {
            cw->restore_pending = true;
            cw->restore_attempted = false;
            cw->restore_phos_cache_rel = oc.project_phos_cache_rel;
            cw->restore_error.clear();

            // Provide a sane blank canvas until the cached project is loaded.
            cw->canvas.SetColumns(80);
            cw->canvas.EnsureRowsPublic(25);
        }
        else
        {
            // Legacy embedded restore.
            AnsiCanvas::ProjectState ps;
            std::string derr;
            if (open_canvas_codec::DecodeProjectState(oc, ps, derr))
            {
                std::string apply_err;
                if (!cw->canvas.SetProjectState(ps, apply_err))
                    std::fprintf(stderr, "[session] restore canvas %d: %s\n", cw->id, apply_err.c_str());
                else
                    cw->canvas.MarkSaved();
            }
            else if (!oc.project_cbor_zstd_b64.empty())
            {
                std::fprintf(stderr, "[session] restore canvas %d: %s\n", cw->id, derr.c_str());
            }
        }

        // Per-canvas background (do this early so the placeholder canvas matches too).
        // Legacy sessions (no per-canvas field) will use the global default.
        cw->canvas.SetCanvasBackgroundWhite(oc.canvas_bg_white || session_state.canvas_bg_white);

        cw->canvas.SetZoom(oc.zoom);
        cw->canvas.RequestScrollPixels(oc.scroll_x, oc.scroll_y);

        const int restored_id = cw->id;
        canvases.push_back(std::move(cw));
        next_canvas_id = std::max(next_canvas_id, restored_id + 1);
    }

    // Restore images (paths only; pixels reloaded).
    for (const auto& oi : session_state.open_images)
    {
        ImageWindow img;
        img.open = oi.open;
        img.id = (oi.id > 0) ? oi.id : next_image_id++;
        img.path = oi.path;
        if (!img.path.empty())
        {
            int iw = 0, ih = 0;
            std::vector<unsigned char> rgba;
            std::string ierr;
            if (image_loader::LoadImageAsRgba32(img.path, iw, ih, rgba, ierr))
            {
                img.width = iw;
                img.height = ih;
                img.pixels = std::move(rgba);
            }
        }
        images.push_back(std::move(img));
        next_image_id = std::max(next_image_id, images.back().id + 1);
    }
}

void SaveSessionStateOnExit(const SessionState& session_state,
                            SDL_Window* window,
                            const IoManager& io_manager,
                            const ToolPalette& tool_palette,
                            const AnslEditor& ansl_editor,
                            bool show_color_picker_window,
                            bool show_character_picker_window,
                            bool show_character_palette_window,
                            bool show_character_sets_window,
                            bool show_layer_manager_window,
                            bool show_ansl_editor_window,
                            bool show_tool_palette_window,
                            bool show_brush_palette_window,
                            bool show_minimap_window,
                            bool show_settings_window,
                            bool show_16colors_browser_window,
                            const ImVec4& fg_color,
                            const ImVec4& bg_color,
                            int active_fb,
                            int xterm_picker_mode,
                            int xterm_selected_palette,
                            int xterm_picker_preview_fb,
                            float xterm_picker_last_hue,
                            int last_active_canvas_id,
                            int next_canvas_id,
                            int next_image_id,
                            const std::vector<std::unique_ptr<CanvasWindow>>& canvases,
                            const std::vector<ImageWindow>& images)
{
    SessionState st = session_state; // start from loaded defaults

    int sw = 0, sh = 0;
    SDL_GetWindowSize(window, &sw, &sh);
    st.window_w = sw;
    st.window_h = sh;

    int sx = 0, sy = 0;
    SDL_GetWindowPosition(window, &sx, &sy);
    st.window_x = sx;
    st.window_y = sy;
    st.window_pos_valid = true;

    const SDL_WindowFlags wf = SDL_GetWindowFlags(window);
    st.window_maximized = (wf & SDL_WINDOW_MAXIMIZED) != 0;
    st.window_fullscreen = (wf & SDL_WINDOW_FULLSCREEN) != 0;

    st.show_color_picker_window = show_color_picker_window;
    st.show_character_picker_window = show_character_picker_window;
    st.show_character_palette_window = show_character_palette_window;
    st.show_character_sets_window = show_character_sets_window;
    st.show_layer_manager_window = show_layer_manager_window;
    st.show_ansl_editor_window = show_ansl_editor_window;
    st.show_tool_palette_window = show_tool_palette_window;
    st.show_brush_palette_window = show_brush_palette_window;
    st.show_minimap_window = show_minimap_window;
    st.show_settings_window = show_settings_window;
    st.show_16colors_browser_window = show_16colors_browser_window;

    // Xterm-256 picker UI state
    st.xterm_color_picker.fg[0] = fg_color.x;
    st.xterm_color_picker.fg[1] = fg_color.y;
    st.xterm_color_picker.fg[2] = fg_color.z;
    st.xterm_color_picker.fg[3] = fg_color.w;
    st.xterm_color_picker.bg[0] = bg_color.x;
    st.xterm_color_picker.bg[1] = bg_color.y;
    st.xterm_color_picker.bg[2] = bg_color.z;
    st.xterm_color_picker.bg[3] = bg_color.w;
    st.xterm_color_picker.active_fb = active_fb;
    st.xterm_color_picker.picker_mode = xterm_picker_mode;
    st.xterm_color_picker.selected_palette = xterm_selected_palette;
    st.xterm_color_picker.picker_preview_fb = xterm_picker_preview_fb;
    st.xterm_color_picker.last_hue = xterm_picker_last_hue;

    st.last_import_image_dir = io_manager.GetLastDir();

    // Active tool
    if (const ToolSpec* t = tool_palette.GetActiveTool())
        st.active_tool_path = t->path;

    // ANSL editor state
    st.ansl_editor.target_fps = ansl_editor.TargetFps();
    st.ansl_editor.selected_example_index = ansl_editor.SelectedExampleIndex();
    st.ansl_editor.selected_example_label = ansl_editor.SelectedExampleLabel();
    st.ansl_editor.selected_example_path = ansl_editor.SelectedExamplePath();
    st.ansl_editor.text_valid = true;
    st.ansl_editor.text = ansl_editor.Text();

    // Canvas/image workspace
    st.last_active_canvas_id = last_active_canvas_id;
    st.next_canvas_id = next_canvas_id;
    st.next_image_id = next_image_id;

    st.open_canvases.clear();
    st.open_canvases.reserve(canvases.size());
    std::vector<std::string> keep_session_canvas_cache;
    keep_session_canvas_cache.reserve(canvases.size());
    for (const auto& cw_ptr : canvases)
    {
        if (!cw_ptr)
            continue;
        const CanvasWindow& cw = *cw_ptr;
        SessionState::OpenCanvas oc;
        oc.id = cw.id;
        oc.open = cw.open;
        oc.file_path = cw.canvas.GetFilePath();
        oc.zoom = cw.canvas.GetZoom();
        oc.canvas_bg_white = cw.canvas.IsCanvasBackgroundWhite();
        oc.active_glyph_cp = cw.canvas.GetActiveGlyphCodePoint();
        oc.active_glyph_utf8 = cw.canvas.GetActiveGlyphUtf8();
        const auto& vs = cw.canvas.GetLastViewState();
        if (vs.valid)
        {
            oc.scroll_x = vs.scroll_x;
            oc.scroll_y = vs.scroll_y;
        }

        // Prefer caching session canvas state as a .phos project under <config>/cache/,
        // and store only the cache path in session.json.
        //
        // IMPORTANT: If the canvas is still pending restore (never loaded), do NOT
        // overwrite the cache file with a blank placeholder.
        if (cw.restore_pending && !cw.restore_attempted && !cw.restore_phos_cache_rel.empty())
        {
            oc.project_phos_cache_rel = cw.restore_phos_cache_rel;
            keep_session_canvas_cache.push_back(oc.project_phos_cache_rel);
        }
        else
        {
            std::string cache_err;
            std::string rel;
            if (open_canvas_cache::SaveCanvasToSessionCachePhos(cw.id, cw.canvas, rel, cache_err))
            {
                oc.project_phos_cache_rel = rel;
                keep_session_canvas_cache.push_back(oc.project_phos_cache_rel);
            }
            else
            {
                // Fall back to legacy embedded payload so we don't lose work if cache IO fails.
                std::string enc_err;
                if (!open_canvas_codec::EncodeProjectState(cw.canvas.GetProjectState(), oc, enc_err))
                {
                    std::fprintf(stderr, "[session] encode canvas %d failed: %s\n", cw.id, enc_err.c_str());
                }
                else
                {
                    std::fprintf(stderr, "[session] cache save canvas %d failed: %s (embedded as fallback)\n",
                                 cw.id, cache_err.c_str());
                }
            }
        }

        st.open_canvases.push_back(std::move(oc));
    }
    open_canvas_cache::PruneSessionCanvasCache(keep_session_canvas_cache);

    st.open_images.clear();
    st.open_images.reserve(images.size());
    for (const auto& im : images)
    {
        SessionState::OpenImage oi;
        oi.id = im.id;
        oi.open = im.open;
        oi.path = im.path;
        if (!oi.path.empty())
            st.open_images.push_back(std::move(oi));
    }

    std::string err;
    if (!SaveSessionState(st, err) && !err.empty())
        std::fprintf(stderr, "[session] save failed: %s\n", err.c_str());
}

} // namespace workspace_persist


