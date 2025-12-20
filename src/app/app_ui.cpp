#include "app/app_ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "imgui.h"

#include "core/paths.h"
#include "core/xterm256_palette.h"
#include "io/formats/plaintext.h"
#include "io/session/session_state.h"

namespace appui
{

std::string ShortcutForAction(const kb::KeyBindingsEngine& keybinds,
                              std::string_view action_id,
                              std::string_view preferred_context)
{
    const kb::Platform plat = kb::RuntimePlatform();
    const auto& actions = keybinds.Actions();
    for (const auto& a : actions)
    {
        if (a.id != action_id)
            continue;

        auto plat_ok = [&](const kb::KeyBinding& b) -> bool
        {
            if (b.platform == "any") return true;
            if (plat == kb::Platform::Windows) return b.platform == "windows";
            if (plat == kb::Platform::Linux) return b.platform == "linux";
            if (plat == kb::Platform::MacOS) return b.platform == "macos";
            return false;
        };

        auto pick = [&](std::string_view ctx) -> std::string
        {
            for (const auto& b : a.bindings)
            {
                if (!b.enabled) continue;
                if (b.chord.empty()) continue;
                if (!plat_ok(b)) continue;
                if (b.context == ctx)
                    return b.chord;
            }
            return {};
        };

        std::string s = pick(preferred_context);
        if (!s.empty()) return s;
        s = pick("global");
        if (!s.empty()) return s;
        for (const auto& b : a.bindings)
        {
            if (!b.enabled) continue;
            if (b.chord.empty()) continue;
            if (plat_ok(b))
                return b.chord;
        }
        return {};
    }
    return {};
}

static int RequestedTopMenu(kb::KeyBindingsEngine& keybinds)
{
    // Menu keyboard navigation:
    // - Alt+F / Alt+E / Alt+W open the respective top-level menu.
    // - We intentionally do NOT use F10, because F1..F12 (including F10) are reserved for character sets.
    int requested_top_menu = 0; // 1=File, 2=Edit, 3=Window
    kb::EvalContext mctx;
    mctx.global = true;
    mctx.platform = kb::RuntimePlatform();
    if (keybinds.ActionPressed("menu.open.file", mctx)) requested_top_menu = 1;
    if (keybinds.ActionPressed("menu.open.edit", mctx)) requested_top_menu = 2;
    if (keybinds.ActionPressed("menu.open.window", mctx)) requested_top_menu = 3;
    return requested_top_menu;
}

void RenderMainMenuBar(SDL_Window* window,
                       kb::KeyBindingsEngine& keybinds,
                       SessionState& session_state,
                       IoManager& io_manager,
                       SdlFileDialogQueue& file_dialogs,
                       IoManager::Callbacks& io_callbacks,
                       ExportDialog& export_dialog,
                       SettingsWindow& settings_window,
                       AnsiCanvas* active_canvas,
                       bool& done,
                       bool& window_fullscreen,
                       bool& show_color_picker_window,
                       bool& show_character_picker_window,
                       bool& show_character_palette_window,
                       bool& show_character_sets_window,
                       bool& show_layer_manager_window,
                       bool& show_ansl_editor_window,
                       bool& show_tool_palette_window,
                       bool& show_minimap_window,
                       bool& show_settings_window,
                       bool& show_16colors_browser_window,
                       const std::function<void()>& create_new_canvas)
{
    const int requested_top_menu = RequestedTopMenu(keybinds);

    if (!ImGui::BeginMainMenuBar())
        return;

    if (requested_top_menu == 1)
        ImGui::OpenPopup("File");
    if (ImGui::BeginMenu("File"))
    {
        const std::string sc_new = ShortcutForAction(keybinds, "app.file.new", "global");
        if (ImGui::MenuItem("New Canvas", sc_new.empty() ? nullptr : sc_new.c_str()))
            create_new_canvas();

        // Project IO + import/export (handled by IoManager).
        io_manager.RenderFileMenu(window, file_dialogs, active_canvas, io_callbacks, [&](std::string_view action_id) {
            return ShortcutForAction(keybinds, action_id, "global");
        });

        // Unified Export menu (all formats share one tabbed dialog).
        if (ImGui::BeginMenu("Export"))
        {
            const std::string sc_e_ansi = ShortcutForAction(keybinds, "app.file.export_ansi", "global");
            const std::string sc_e_png = ShortcutForAction(keybinds, "app.file.export_png", "global");
            const std::string sc_e_utf8 = ShortcutForAction(keybinds, "app.file.export_utf8", "global");

            if (ImGui::MenuItem("ANSI…", sc_e_ansi.empty() ? nullptr : sc_e_ansi.c_str()))
                export_dialog.Open(ExportDialog::Tab::Ansi);
            if (ImGui::MenuItem("Plaintext…", sc_e_utf8.empty() ? nullptr : sc_e_utf8.c_str()))
                export_dialog.Open(ExportDialog::Tab::Plaintext);
            if (ImGui::MenuItem("Image…", sc_e_png.empty() ? nullptr : sc_e_png.c_str()))
                export_dialog.Open(ExportDialog::Tab::Image);
            if (ImGui::MenuItem("XBin…"))
                export_dialog.Open(ExportDialog::Tab::XBin);
            ImGui::EndMenu();
        }

        const std::string sc_quit = ShortcutForAction(keybinds, "app.quit", "global");
        if (ImGui::MenuItem("Quit", sc_quit.empty() ? nullptr : sc_quit.c_str()))
            done = true;

        ImGui::Separator();
        const std::string sc_settings = ShortcutForAction(keybinds, "app.settings.open", "global");
        if (ImGui::MenuItem("Settings...", sc_settings.empty() ? nullptr : sc_settings.c_str()))
        {
            show_settings_window = true;
            settings_window.SetOpen(true);
        }

        ImGui::EndMenu();
    }

    if (requested_top_menu == 2)
        ImGui::OpenPopup("Edit");
    if (ImGui::BeginMenu("Edit"))
    {
        // Use the active canvas so clicking the menu bar doesn't make Undo/Redo unavailable.
        const bool can_undo = active_canvas && active_canvas->CanUndo();
        const bool can_redo = active_canvas && active_canvas->CanRedo();

        const std::string sc_undo = ShortcutForAction(keybinds, "edit.undo", "editor");
        const std::string sc_redo = ShortcutForAction(keybinds, "edit.redo", "editor");

        if (ImGui::MenuItem("Undo", sc_undo.empty() ? nullptr : sc_undo.c_str(), false, can_undo))
            active_canvas->Undo();
        if (ImGui::MenuItem("Redo", sc_redo.empty() ? nullptr : sc_redo.c_str(), false, can_redo))
            active_canvas->Redo();

        ImGui::EndMenu();
    }

    if (requested_top_menu == 3)
        ImGui::OpenPopup("Window");
    if (ImGui::BeginMenu("Window"))
    {
        ImGui::MenuItem("Colour Picker", nullptr, &show_color_picker_window);
        ImGui::MenuItem("Unicode Character Picker", nullptr, &show_character_picker_window);
        ImGui::MenuItem("Character Palette", nullptr, &show_character_palette_window);
        ImGui::MenuItem("Character Sets", nullptr, &show_character_sets_window);
        ImGui::MenuItem("Layer Manager", nullptr, &show_layer_manager_window);
        ImGui::MenuItem("ANSL Editor", nullptr, &show_ansl_editor_window);
        ImGui::MenuItem("Tool Palette", nullptr, &show_tool_palette_window);
        ImGui::MenuItem("Minimap", nullptr, &show_minimap_window);
        ImGui::MenuItem("16colo.rs Browser", nullptr, &show_16colors_browser_window);
        ImGui::Separator();
        if (ImGui::MenuItem("Fullscreen", nullptr, &window_fullscreen))
        {
            if (!SDL_SetWindowFullscreen(window, window_fullscreen))
            {
                // Revert UI state if the window manager denies the request.
                window_fullscreen = !window_fullscreen;
            }
            else
            {
                // Persist immediately in-memory; file is written at shutdown.
                session_state.window_fullscreen = window_fullscreen;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void HandleKeybindings(SDL_Window* window,
                       kb::KeyBindingsEngine& keybinds,
                       SessionState& session_state,
                       IoManager& io_manager,
                       SdlFileDialogQueue& file_dialogs,
                       ExportDialog& export_dialog,
                       ToolPalette& tool_palette,
                       const std::function<void(const std::string& tool_path)>& compile_tool_script,
                       const std::function<void()>& sync_tool_stack,
                       AnsiCanvas* focused_canvas,
                       CanvasWindow* focused_canvas_window,
                       AnsiCanvas* active_canvas,
                       CanvasWindow* active_canvas_window,
                       bool& done,
                       bool& window_fullscreen,
                       bool& show_minimap_window,
                       bool& show_settings_window,
                       SettingsWindow& settings_window,
                       ImVec4& fg_color,
                       ImVec4& bg_color,
                       const std::function<void()>& create_new_canvas)
{
    const bool any_popup =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    kb::EvalContext kctx;
    kctx.global = true;
    kctx.editor = (focused_canvas != nullptr);
    kctx.canvas = (focused_canvas != nullptr);
    kctx.selection = (focused_canvas != nullptr && focused_canvas->HasSelection());
    kctx.platform = kb::RuntimePlatform();

    // Settings window hotkey is truly global (no focused canvas required).
    if (!any_popup && keybinds.ActionPressed("app.settings.open", kctx))
    {
        show_settings_window = true;
        settings_window.SetOpen(true);
    }

    // File-level actions (no focused canvas required; Save is gated below).
    if (!any_popup)
    {
        if (keybinds.ActionPressed("app.file.new", kctx))
            create_new_canvas();

        if (keybinds.ActionPressed("app.file.open", kctx))
            io_manager.RequestLoadFile(window, file_dialogs);

        const bool save_pressed =
            keybinds.ActionPressed("app.file.save", kctx) ||
            keybinds.ActionPressed("app.file.save_as", kctx);
        if (save_pressed && active_canvas)
            io_manager.RequestSaveProject(window, file_dialogs);

        if (keybinds.ActionPressed("app.file.export_ansi", kctx) && active_canvas)
            export_dialog.Open(ExportDialog::Tab::Ansi);

        if (keybinds.ActionPressed("app.file.export_png", kctx) && active_canvas)
            export_dialog.Open(ExportDialog::Tab::Image);

        if (keybinds.ActionPressed("app.file.export_apng", kctx) && active_canvas)
            export_dialog.Open(ExportDialog::Tab::Image);

        if (keybinds.ActionPressed("app.file.export_utf8", kctx) && active_canvas)
            export_dialog.OpenPlaintextPreset(formats::plaintext::PresetId::PlainUtf8);

        // SAUCE editor dialog (canvas-scoped but opened via File hotkey).
        if (keybinds.ActionPressed("app.file.edit_sauce", kctx))
        {
            CanvasWindow* target = focused_canvas_window ? focused_canvas_window : active_canvas_window;
            if (target)
                target->sauce_dialog.OpenFromCanvas(target->canvas);
        }

        if (keybinds.ActionPressed("app.file.close_window", kctx))
        {
            if (focused_canvas_window)
                focused_canvas_window->open = false;
            else if (active_canvas_window)
                active_canvas_window->open = false;
            else
                done = true;
        }

        if (keybinds.ActionPressed("app.quit", kctx))
            done = true;

        // Global view/UI toggles (typically disabled by default in key-bindings.json).
        if (keybinds.ActionPressed("view.fullscreen_toggle", kctx))
        {
            window_fullscreen = !window_fullscreen;
            if (!SDL_SetWindowFullscreen(window, window_fullscreen))
                window_fullscreen = !window_fullscreen;
            else
                session_state.window_fullscreen = window_fullscreen;
        }
        if (keybinds.ActionPressed("ui.toggle_preview", kctx))
            show_minimap_window = !show_minimap_window;
        if (keybinds.ActionPressed("ui.toggle_status_bar", kctx))
        {
            if (focused_canvas)
                focused_canvas->ToggleStatusLineVisible();
            else if (active_canvas)
                active_canvas->ToggleStatusLineVisible();
        }
    }

    // Canvas-scoped edit/view shortcuts: only when a canvas grid is focused.
    if (focused_canvas && !any_popup)
    {
        if (keybinds.ActionPressed("edit.undo", kctx))
            focused_canvas->Undo();
        if (keybinds.ActionPressed("edit.redo", kctx))
            focused_canvas->Redo();

        // Zoom via keybindings (mouse wheel zoom remains implemented in AnsiCanvas).
        if (keybinds.ActionPressed("view.zoom_in", kctx))
            focused_canvas->SetZoom(focused_canvas->GetZoom() * 1.10f);
        if (keybinds.ActionPressed("view.zoom_out", kctx))
            focused_canvas->SetZoom(focused_canvas->GetZoom() / 1.10f);
        if (keybinds.ActionPressed("view.zoom_reset", kctx))
            focused_canvas->SetZoom(1.0f);
        if (keybinds.ActionPressed("view.actual_size", kctx))
            focused_canvas->SetZoom(1.0f);

        // Scroll controls (optional / disabled by default).
        if (keybinds.ActionPressed("view.toggle_scroll_with_cursor", kctx))
            focused_canvas->ToggleFollowCaretEnabled();
        if (keybinds.ActionPressed("view.scroll_up", kctx) ||
            keybinds.ActionPressed("view.scroll_down", kctx) ||
            keybinds.ActionPressed("view.scroll_left", kctx) ||
            keybinds.ActionPressed("view.scroll_right", kctx))
        {
            const auto& vs = focused_canvas->GetLastViewState();
            float sx = vs.valid ? vs.scroll_x : 0.0f;
            float sy = vs.valid ? vs.scroll_y : 0.0f;
            const float step_x = (vs.valid && vs.cell_w > 0.0f) ? (vs.cell_w * 4.0f) : 64.0f;
            const float step_y = (vs.valid && vs.cell_h > 0.0f) ? (vs.cell_h * 2.0f) : 48.0f;

            if (keybinds.ActionPressed("view.scroll_up", kctx)) sy -= step_y;
            if (keybinds.ActionPressed("view.scroll_down", kctx)) sy += step_y;
            if (keybinds.ActionPressed("view.scroll_left", kctx)) sx -= step_x;
            if (keybinds.ActionPressed("view.scroll_right", kctx)) sx += step_x;
            if (sx < 0.0f) sx = 0.0f;
            if (sy < 0.0f) sy = 0.0f;
            focused_canvas->RequestScrollPixels(sx, sy);
        }

        // Colour hotkeys affect the shared fg/bg selection used by tools.
        auto to_idx = [](const ImVec4& c) -> int {
            const int r = (int)std::lround(c.x * 255.0f);
            const int g = (int)std::lround(c.y * 255.0f);
            const int b = (int)std::lround(c.z * 255.0f);
            return xterm256::NearestIndex((std::uint8_t)std::clamp(r, 0, 255),
                                          (std::uint8_t)std::clamp(g, 0, 255),
                                          (std::uint8_t)std::clamp(b, 0, 255));
        };
        auto apply_idx_to_color = [](int idx, ImVec4& dst) {
            idx = std::clamp(idx, 0, 255);
            const xterm256::Rgb rgb = xterm256::RgbForIndex(idx);
            dst.x = (float)rgb.r / 255.0f;
            dst.y = (float)rgb.g / 255.0f;
            dst.z = (float)rgb.b / 255.0f;
            dst.w = 1.0f;
        };

        if (keybinds.ActionPressed("color.prev_fg", kctx))
        {
            int idx = to_idx(fg_color);
            idx = (idx + 255) % 256;
            apply_idx_to_color(idx, fg_color);
        }
        if (keybinds.ActionPressed("color.next_fg", kctx))
        {
            int idx = to_idx(fg_color);
            idx = (idx + 1) % 256;
            apply_idx_to_color(idx, fg_color);
        }
        if (keybinds.ActionPressed("color.prev_bg", kctx))
        {
            int idx = to_idx(bg_color);
            idx = (idx + 255) % 256;
            apply_idx_to_color(idx, bg_color);
        }
        if (keybinds.ActionPressed("color.next_bg", kctx))
        {
            int idx = to_idx(bg_color);
            idx = (idx + 1) % 256;
            apply_idx_to_color(idx, bg_color);
        }
        if (keybinds.ActionPressed("color.default", kctx))
        {
            apply_idx_to_color(7, fg_color);
            apply_idx_to_color(0, bg_color);
        }
        if (keybinds.ActionPressed("color.pick_attribute", kctx))
        {
            int cx = 0, cy = 0;
            focused_canvas->GetCaretCell(cx, cy);
            char32_t cp = U' ';
            AnsiCanvas::Color32 fg = 0;
            AnsiCanvas::Color32 bg = 0;
            if (focused_canvas->GetCompositeCellPublic(cy, cx, cp, fg, bg))
            {
                if (fg != 0) apply_idx_to_color((int)fg, fg_color);
                if (bg != 0) apply_idx_to_color((int)bg, bg_color);
            }
        }

        // Tool switching (selection).
        if (keybinds.ActionPressed("selection.start_block", kctx))
        {
            namespace fs = std::filesystem;
            const std::string tools_dir =
                tool_palette.GetToolsDir().empty() ? PhosphorAssetPath("tools") : tool_palette.GetToolsDir();
            const std::string select_path = (fs::path(tools_dir) / "select.lua").string();
            if (!select_path.empty() && tool_palette.SetActiveToolByPath(select_path))
            {
                compile_tool_script(select_path);
                sync_tool_stack();
            }
        }
    }
}

} // namespace appui


