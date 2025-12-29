#include "app/app_ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"
#include <nlohmann/json.hpp>

#include "app/action_execute.h"
#include "app/clipboard_utils.h"
#include "core/colour_ops.h"
#include "core/colour_system.h"
#include "core/i18n.h"
#include "core/paths.h"
#include "io/formats/plaintext.h"
#include "io/session/session_state.h"

namespace appui
{

struct TutorialEntry
{
    std::string filename;
    std::string title;
    std::string artist;
    std::optional<int> released_year;
};

static bool LoadTutorialsFromJson(const std::string& path,
                                 std::vector<TutorialEntry>& out,
                                 std::string& err)
{
    using nlohmann::json;

    err.clear();
    out.clear();

    std::ifstream f(path);
    if (!f)
    {
        err = PHOS_TRF("menu.file.tutorials_errors.failed_to_open_fmt", phos::i18n::Arg::Str(path));
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }

    if (!j.is_array())
    {
        err = PHOS_TR("menu.file.tutorials_errors.expected_top_level_array");
        return false;
    }

    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        TutorialEntry t;

        if (auto it = item.find("filename"); it != item.end() && it->is_string())
            t.filename = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("title"); it != item.end() && it->is_string())
            t.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("artist"); it != item.end() && it->is_string())
            t.artist = it->get<std::string>();
        else
            t.artist = PHOS_TR("common.unknown");

        if (auto it = item.find("released_year"); it != item.end())
        {
            if (it->is_number_integer())
                t.released_year = it->get<int>();
            else
                t.released_year.reset(); // null/invalid
        }

        out.push_back(std::move(t));
    }

    if (out.empty())
    {
        err = PHOS_TR("menu.file.tutorials_errors.no_valid_tutorials");
        return false;
    }

    return true;
}

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
    // - Alt+F / Alt+E / Alt+V / Alt+W open the respective top-level menu.
    // - We intentionally do NOT use F10, because F1..F12 (including F10) are reserved for character sets.
    //
    // Transitional focus/input safety:
    // If the user is typing into an ImGui text widget, do NOT allow menu-open chords to fire.
    // (This aligns with the "WantTextInput dominance" rule in the focus/router refactor docs.)
    if (ImGui::GetIO().WantTextInput)
        return 0;

    int requested_top_menu = 0; // 1=File, 2=Edit, 3=View, 4=Window
    kb::EvalContext mctx;
    mctx.global = true;
    mctx.platform = kb::RuntimePlatform();

    // Preserve existing priority/override behavior: later checks override earlier ones.
    if (keybinds.ActionPressed("menu.open.file", mctx)) requested_top_menu = 1;
    if (keybinds.ActionPressed("menu.open.edit", mctx)) requested_top_menu = 2;
    if (keybinds.ActionPressed("menu.open.view", mctx)) requested_top_menu = 3;
    if (keybinds.ActionPressed("menu.open.window", mctx)) requested_top_menu = 4;
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
                       bool& show_colour_picker_window,
                       bool& show_character_picker_window,
                       bool& show_character_palette_window,
                       bool& show_character_sets_window,
                       bool& show_layer_manager_window,
                       bool& show_ansl_editor_window,
                       bool& show_tool_palette_window,
                       bool& show_tool_presets_window,
                       bool& show_brush_palette_window,
                       bool& show_minimap_window,
                       bool& show_settings_window,
                       bool& show_16colors_browser_window,
                       const std::function<void()>& create_new_canvas)
{
    const int requested_top_menu = RequestedTopMenu(keybinds);

    if (!ImGui::BeginMainMenuBar())
        return;

    // Localized visible labels + stable ImGui IDs (stable across language changes).
    const std::string m_file = PHOS_TR("menu.top.file") + "###menu_file";
    const std::string m_edit = PHOS_TR("menu.top.edit") + "###menu_edit";
    const std::string m_view = PHOS_TR("menu.top.view") + "###menu_view";
    const std::string m_window = PHOS_TR("menu.top.window") + "###menu_window";

    if (requested_top_menu == 1)
        ImGui::OpenPopup(m_file.c_str());
    if (ImGui::BeginMenu(m_file.c_str()))
    {
        const std::string sc_new = ShortcutForAction(keybinds, "app.file.new", "global");
        const std::string mi_new_canvas = PHOS_TR("menu.file.new_canvas");
        if (ImGui::MenuItem(mi_new_canvas.c_str(), sc_new.empty() ? nullptr : sc_new.c_str()))
            create_new_canvas();

        // Project IO + import/export (handled by IoManager).
        io_manager.RenderFileMenu(window, file_dialogs, active_canvas, io_callbacks, [&](std::string_view action_id) {
            return ShortcutForAction(keybinds, action_id, "global");
        });

        // Recent file list (persisted in session.json).
        const std::string m_recent = PHOS_TR("menu.file.recent") + "###menu_recent";
        if (ImGui::BeginMenu(m_recent.c_str()))
        {
            auto sanitize = [](std::string s) -> std::string
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

            if (session_state.recent_files.empty())
            {
                ImGui::BeginDisabled();
                const std::string mi_recent_empty = PHOS_TR("menu.file.recent_empty");
                ImGui::MenuItem(mi_recent_empty.c_str());
                ImGui::EndDisabled();
            }
            else
            {
                namespace fs = std::filesystem;
                for (size_t i = 0; i < session_state.recent_files.size(); ++i)
                {
                    const std::string& p = session_state.recent_files[i];
                    const bool is_uri = (p.find("://") != std::string::npos);
                    bool exists = true;
                    if (!is_uri)
                    {
                        std::error_code ec;
                        exists = fs::exists(fs::path(p), ec) && !ec;
                    }

                    std::string label = sanitize(p);
                    if (!exists && !is_uri)
                        label += PHOS_TR("app_strings.missing_suffix");

                    if (ImGui::MenuItem(label.c_str(), nullptr, false, exists || is_uri))
                        io_manager.OpenPath(p, io_callbacks, &session_state);
                }
            }

            ImGui::Separator();
            const std::string mi_recent_clear = PHOS_TR("menu.file.recent_clear");
            if (ImGui::MenuItem(mi_recent_clear.c_str()))
                session_state.recent_files.clear();

            ImGui::EndMenu();
        }

        // Tutorials (bundled assets/ansi-tutorials/* + assets/ansi-tutorials.json).
        const std::string m_tutorials = PHOS_TR("menu.file.tutorials") + "###menu_tutorials";
        if (ImGui::BeginMenu(m_tutorials.c_str()))
        {
            auto sanitize = [](std::string s) -> std::string
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

            static bool tutorials_loaded = false;
            static std::vector<TutorialEntry> tutorials;
            static std::string tutorials_error;

            if (!tutorials_loaded)
            {
                const std::string json_path = PhosphorAssetPath("ansi-tutorials.json");
                if (!LoadTutorialsFromJson(json_path, tutorials, tutorials_error))
                    tutorials.clear();
                tutorials_loaded = true;
            }

            if (!tutorials_error.empty())
            {
                ImGui::BeginDisabled();
                const std::string mi_tutorials_failed = PHOS_TR("menu.file.tutorials_failed");
                ImGui::MenuItem(mi_tutorials_failed.c_str());
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", tutorials_error.c_str());
            }
            else if (tutorials.empty())
            {
                ImGui::BeginDisabled();
                const std::string mi_tutorials_empty = PHOS_TR("menu.file.tutorials_empty");
                ImGui::MenuItem(mi_tutorials_empty.c_str());
                ImGui::EndDisabled();
            }
            else
            {
                namespace fs = std::filesystem;
                for (size_t i = 0; i < tutorials.size(); ++i)
                {
                    const TutorialEntry& t = tutorials[i];
                    const std::string path = (fs::path(PhosphorAssetPath("ansi-tutorials")) / t.filename).string();

                    std::string label = t.title + " by " + t.artist + " (";
                    const std::string year = t.released_year.has_value()
                        ? std::to_string(*t.released_year)
                        : PHOS_TR("common.unknown_short");
                    label = PHOS_TRF("menu.file.tutorial_entry_fmt",
                                     phos::i18n::Arg::Str(t.title),
                                     phos::i18n::Arg::Str(t.artist),
                                     phos::i18n::Arg::Str(year));
                    label = sanitize(std::move(label));

                    std::error_code ec;
                    const bool exists = fs::exists(fs::path(path), ec) && !ec;
                    if (!exists)
                        label += PHOS_TR("app_strings.missing_suffix");

                    if (ImGui::MenuItem(label.c_str(), nullptr, false, exists))
                        io_manager.OpenPath(path, io_callbacks, &session_state);
                }
            }

            ImGui::EndMenu();
        }

        // Unified Export menu (all formats share one tabbed dialog).
        const std::string m_export = PHOS_TR("menu.file.export") + "###menu_export";
        if (ImGui::BeginMenu(m_export.c_str()))
        {
            const std::string sc_e_ansi = ShortcutForAction(keybinds, "app.file.export_ansi", "global");
            const std::string sc_e_png = ShortcutForAction(keybinds, "app.file.export_png", "global");
            const std::string sc_e_utf8 = ShortcutForAction(keybinds, "app.file.export_utf8", "global");

            const std::string mi_export_ansi = PHOS_TR("menu.file.export_ansi_ellipsis");
            const std::string mi_export_plain = PHOS_TR("menu.file.export_plaintext_ellipsis");
            const std::string mi_export_image = PHOS_TR("menu.file.export_image_ellipsis");
            const std::string mi_export_xbin = PHOS_TR("menu.file.export_xbin_ellipsis");

            if (ImGui::MenuItem(mi_export_ansi.c_str(), sc_e_ansi.empty() ? nullptr : sc_e_ansi.c_str()))
                export_dialog.Open(ExportDialog::Tab::Ansi);
            if (ImGui::MenuItem(mi_export_plain.c_str(), sc_e_utf8.empty() ? nullptr : sc_e_utf8.c_str()))
                export_dialog.Open(ExportDialog::Tab::Plaintext);
            if (ImGui::MenuItem(mi_export_image.c_str(), sc_e_png.empty() ? nullptr : sc_e_png.c_str()))
                export_dialog.Open(ExportDialog::Tab::Image);
            if (ImGui::MenuItem(mi_export_xbin.c_str()))
                export_dialog.Open(ExportDialog::Tab::XBin);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        const std::string sc_settings = ShortcutForAction(keybinds, "app.settings.open", "global");
        const std::string mi_settings = PHOS_TR("menu.file.settings_ellipsis");
        if (ImGui::MenuItem(mi_settings.c_str(), sc_settings.empty() ? nullptr : sc_settings.c_str()))
        {
            show_settings_window = true;
            settings_window.SetOpen(true);
        }

        ImGui::Separator();
        const std::string sc_quit = ShortcutForAction(keybinds, "app.quit", "global");
        const std::string mi_quit = PHOS_TR("menu.file.quit");
        if (ImGui::MenuItem(mi_quit.c_str(), sc_quit.empty() ? nullptr : sc_quit.c_str()))
            done = true;

        ImGui::EndMenu();
    }

    if (requested_top_menu == 2)
        ImGui::OpenPopup(m_edit.c_str());
    if (ImGui::BeginMenu(m_edit.c_str()))
    {
        // Use the active canvas so clicking the menu bar doesn't make Undo/Redo unavailable.
        const bool can_undo = active_canvas && active_canvas->CanUndo();
        const bool can_redo = active_canvas && active_canvas->CanRedo();
        const bool can_select_all = (active_canvas != nullptr);
        const bool can_select_none = (active_canvas != nullptr) && active_canvas->HasSelection();
        const bool can_copy_cut = (active_canvas != nullptr) && active_canvas->HasSelection();
        const bool can_paste = (active_canvas != nullptr);
        const bool can_delete_selection = (active_canvas != nullptr) && active_canvas->HasSelection();
        // Forward delete is ambiguous when a selection exists; keep menu semantics explicit.
        const bool can_delete_forward = (active_canvas != nullptr) && !active_canvas->HasSelection();

        const std::string sc_undo = ShortcutForAction(keybinds, "edit.undo", "editor");
        const std::string sc_redo = ShortcutForAction(keybinds, "edit.redo", "editor");
        const std::string sc_copy = ShortcutForAction(keybinds, "edit.copy", "editor");
        const std::string sc_cut = ShortcutForAction(keybinds, "edit.cut", "editor");
        const std::string sc_paste = ShortcutForAction(keybinds, "edit.paste", "editor");
        const std::string sc_delete_selection = ShortcutForAction(keybinds, "selection.clear", "selection");
        const std::string sc_delete_forward = ShortcutForAction(keybinds, "editor.delete_forward_shift", "editor");
        const std::string sc_select_all = ShortcutForAction(keybinds, "edit.select_all", "editor");
        const std::string sc_select_none = ShortcutForAction(keybinds, "selection.clear_or_cancel", "selection");
        const std::string sc_mirror = ShortcutForAction(keybinds, "editor.mirror_mode_toggle", "editor");

        const std::string mi_undo = PHOS_TR("menu.edit.undo");
        const std::string mi_redo = PHOS_TR("menu.edit.redo");
        const std::string mi_copy = PHOS_TR("menu.edit.copy");
        const std::string mi_cut = PHOS_TR("menu.edit.cut");
        const std::string mi_paste = PHOS_TR("menu.edit.paste");
        const std::string mi_delete_selection = PHOS_TR("menu.edit.delete_selection");
        const std::string mi_delete_forward = PHOS_TR("menu.edit.delete_forward_shift");
        const std::string mi_select_all = PHOS_TR("menu.edit.select_all");
        const std::string mi_select_none = PHOS_TR("menu.edit.select_none");
        const std::string mi_mirror = PHOS_TR("menu.edit.mirror_mode");

        if (ImGui::MenuItem(mi_undo.c_str(), sc_undo.empty() ? nullptr : sc_undo.c_str(), false, can_undo))
            active_canvas->Undo();
        if (ImGui::MenuItem(mi_redo.c_str(), sc_redo.empty() ? nullptr : sc_redo.c_str(), false, can_redo))
            active_canvas->Redo();

        ImGui::Separator();
        if (ImGui::MenuItem(mi_copy.c_str(), sc_copy.empty() ? nullptr : sc_copy.c_str(), false, can_copy_cut))
        {
            (void)app::CopySelectionToSystemClipboardText(*active_canvas);
            (void)active_canvas->CopySelectionToClipboard();
        }
        if (ImGui::MenuItem(mi_cut.c_str(), sc_cut.empty() ? nullptr : sc_cut.c_str(), false, can_copy_cut))
        {
            (void)app::CopySelectionToSystemClipboardText(*active_canvas);
            (void)active_canvas->CutSelectionToClipboard();
        }
        if (ImGui::MenuItem(mi_paste.c_str(), sc_paste.empty() ? nullptr : sc_paste.c_str(), false, can_paste))
        {
            int cx = 0, cy = 0;
            active_canvas->GetCaretCell(cx, cy);
            if (!app::PasteSystemClipboardText(*active_canvas, cx, cy))
                (void)active_canvas->PasteClipboard(cx, cy);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(mi_delete_selection.c_str(),
                            sc_delete_selection.empty() ? nullptr : sc_delete_selection.c_str(),
                            false,
                            can_delete_selection))
        {
            // If a floating move is in progress, commit it before destructive operations.
            if (active_canvas->IsMovingSelection())
                (void)active_canvas->CommitMoveSelection();
            (void)active_canvas->DeleteSelection();
        }
        if (ImGui::MenuItem(mi_delete_forward.c_str(),
                            sc_delete_forward.empty() ? nullptr : sc_delete_forward.c_str(),
                            false,
                            can_delete_forward))
        {
            (void)active_canvas->DeleteForwardShift();
        }

        ImGui::Separator();
        if (ImGui::MenuItem(mi_select_all.c_str(), sc_select_all.empty() ? nullptr : sc_select_all.c_str(), false, can_select_all))
            active_canvas->SelectAll();
        if (ImGui::MenuItem(mi_select_none.c_str(), sc_select_none.empty() ? nullptr : sc_select_none.c_str(), false, can_select_none))
            active_canvas->ClearSelection();

        ImGui::Separator();
        {
            bool mirror = (active_canvas != nullptr) ? active_canvas->IsMirrorModeEnabled() : false;
            if (ImGui::MenuItem(mi_mirror.c_str(), sc_mirror.empty() ? nullptr : sc_mirror.c_str(), &mirror, active_canvas != nullptr))
                active_canvas->SetMirrorModeEnabled(mirror);
        }

        ImGui::EndMenu();
    }


    if (requested_top_menu == 3)
        ImGui::OpenPopup(m_view.c_str());
    if (ImGui::BeginMenu(m_view.c_str()))
    {
        const bool can_zoom = (active_canvas != nullptr);
        const std::string sc_zoom_in = ShortcutForAction(keybinds, "view.zoom_in", "global");
        const std::string sc_zoom_out = ShortcutForAction(keybinds, "view.zoom_out", "global");
        const std::string sc_zoom_reset = ShortcutForAction(keybinds, "view.zoom_reset", "global");

        const std::string mi_zoom_in = PHOS_TR("menu.view.zoom_in");
        const std::string mi_zoom_out = PHOS_TR("menu.view.zoom_out");
        const std::string mi_zoom_reset = PHOS_TR("menu.view.reset_zoom_1_1");

        if (ImGui::MenuItem(mi_zoom_in.c_str(), sc_zoom_in.empty() ? nullptr : sc_zoom_in.c_str(), false, can_zoom))
            active_canvas->SetZoom(active_canvas->GetZoom() * 1.10f);
        if (ImGui::MenuItem(mi_zoom_out.c_str(), sc_zoom_out.empty() ? nullptr : sc_zoom_out.c_str(), false, can_zoom))
            active_canvas->SetZoom(active_canvas->GetZoom() / 1.10f);
        if (ImGui::MenuItem(mi_zoom_reset.c_str(), sc_zoom_reset.empty() ? nullptr : sc_zoom_reset.c_str(), false, can_zoom))
            active_canvas->SetZoom(1.0f);

        ImGui::EndMenu();
    }

    if (requested_top_menu == 4)
        ImGui::OpenPopup(m_window.c_str());
    if (ImGui::BeginMenu(m_window.c_str()))
    {
        const std::string mi_colour_picker = PHOS_TR("menu.window.colour_picker");
        const std::string mi_unicode_picker = PHOS_TR("menu.window.unicode_character_picker");
        const std::string mi_char_palette = PHOS_TR("menu.window.character_palette");
        const std::string mi_char_sets = PHOS_TR("menu.window.character_sets");
        const std::string mi_layer_mgr = PHOS_TR("menu.window.layer_manager");
        const std::string mi_ansl_editor = PHOS_TR("menu.window.ansl_editor");
        const std::string mi_tool_palette = PHOS_TR("menu.window.tool_palette");
        const std::string mi_tool_presets = PHOS_TR("menu.window.tool_presets");
        const std::string mi_brush_palette = PHOS_TR("menu.window.brush_palette");
        const std::string mi_minimap = PHOS_TR("menu.window.minimap");
        const std::string mi_16c = PHOS_TR("menu.window.sixteen_colors_browser");
        const std::string mi_fullscreen = PHOS_TR("menu.window.fullscreen");

        ImGui::MenuItem(mi_colour_picker.c_str(), nullptr, &show_colour_picker_window);
        ImGui::MenuItem(mi_unicode_picker.c_str(), nullptr, &show_character_picker_window);
        ImGui::MenuItem(mi_char_palette.c_str(), nullptr, &show_character_palette_window);
        ImGui::MenuItem(mi_char_sets.c_str(), nullptr, &show_character_sets_window);
        ImGui::MenuItem(mi_layer_mgr.c_str(), nullptr, &show_layer_manager_window);
        ImGui::MenuItem(mi_ansl_editor.c_str(), nullptr, &show_ansl_editor_window);
        ImGui::MenuItem(mi_tool_palette.c_str(), nullptr, &show_tool_palette_window);
        ImGui::MenuItem(mi_tool_presets.c_str(), nullptr, &show_tool_presets_window);
        ImGui::MenuItem(mi_brush_palette.c_str(), nullptr, &show_brush_palette_window);
        ImGui::MenuItem(mi_minimap.c_str(), nullptr, &show_minimap_window);
        ImGui::MenuItem(mi_16c.c_str(), nullptr, &show_16colors_browser_window);
        ImGui::Separator();
        if (ImGui::MenuItem(mi_fullscreen.c_str(), nullptr, window_fullscreen))
        {
            ImVec4 dummy_fg(0, 0, 0, 1);
            ImVec4 dummy_bg(0, 0, 0, 1);
            const app::ActionExecContext aexec = {
                .window = window,
                .session_state = session_state,
                .io_manager = io_manager,
                .file_dialogs = file_dialogs,
                .export_dialog = export_dialog,
                .settings_window = settings_window,
                .focused_canvas = nullptr,
                .focused_canvas_window = nullptr,
                .active_canvas = active_canvas,
                .active_canvas_window = nullptr,
                .done = done,
                .window_fullscreen = window_fullscreen,
                .show_minimap_window = show_minimap_window,
                .show_settings_window = show_settings_window,
                .fg_colour = dummy_fg, // unused by this action
                .bg_colour = dummy_bg, // unused by this action
                .create_new_canvas = create_new_canvas,
            };
            // NOTE: ExecuteActionId handles toggling + persistence + maximize retry behavior.
            (void)app::ExecuteActionId("view.fullscreen_toggle", aexec);
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
                       ImVec4& fg_colour,
                       ImVec4& bg_colour,
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

    // Centralized keybinding -> action execution for host actions.
    // Tool-claimed actions (and tool-specific behaviors) are handled in RunFrame's tool routing.
    if (!any_popup)
    {
        const app::ActionExecContext aexec = {
            .window = window,
            .session_state = session_state,
            .io_manager = io_manager,
            .file_dialogs = file_dialogs,
            .export_dialog = export_dialog,
            .settings_window = settings_window,
            .focused_canvas = focused_canvas,
            .focused_canvas_window = focused_canvas_window,
            .active_canvas = active_canvas,
            .active_canvas_window = active_canvas_window,
            .done = done,
            .window_fullscreen = window_fullscreen,
            .show_minimap_window = show_minimap_window,
            .show_settings_window = show_settings_window,
            .fg_colour = fg_colour,
            .bg_colour = bg_colour,
            .create_new_canvas = create_new_canvas,
        };

        std::vector<std::string_view> pressed_action_ids;
        keybinds.CollectPressedActions(kctx, pressed_action_ids, 64);
        for (const std::string_view id : pressed_action_ids)
        {
            // Tool switching (selection): keep legacy behavior, but consume it from the same
            // pressed-action list so we don't need a separate ActionPressed() query.
            if (focused_canvas && id == "selection.start_block")
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
                continue;
            }

            (void)app::ExecuteActionId(id, aexec);
        }
    }
}

} // namespace appui


