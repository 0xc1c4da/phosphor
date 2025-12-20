#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "core/key_bindings.h"
#include "io/io_manager.h"
#include "io/sdl_file_dialog_queue.h"
#include "ui/export_dialog.h"
#include "ui/settings.h"
#include "ui/tool_palette.h"

#include "app/workspace.h"

namespace appui
{
// Resolve a human-friendly shortcut string for a keybinding action (best effort).
std::string ShortcutForAction(const kb::KeyBindingsEngine& keybinds,
                              std::string_view action_id,
                              std::string_view preferred_context);

// Render the main menu bar (File/Edit/Window) and drive menu actions.
//
// - `create_new_canvas`: should allocate and push a new CanvasWindow.
// - `io_callbacks`: used by IoManager for opening/importing.
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
                       const std::function<void()>& create_new_canvas);

// Handle app-level keybindings (global + canvas-scoped) that used to live in main.cpp.
//
// Notes:
// - The keybinding engine does not auto-gate based on popups/focus; we do that here.
// - Tool selection hotkey needs a callback so `main.cpp` can keep tool compilation logic local.
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
                       const std::function<void()>& create_new_canvas);

} // namespace appui


