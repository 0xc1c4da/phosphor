#pragma once

#include <memory>
#include <vector>

#include <SDL3/SDL.h>

#include "io/session/session_state.h"

#include "app/workspace.h"
#include "io/io_manager.h"
#include "ui/ansl_editor.h"
#include "ui/image_window.h"
#include "ui/tool_palette.h"

namespace workspace_persist
{

// Restore open canvases + images from SessionState into the in-memory workspace vectors.
void RestoreWorkspaceFromSession(const SessionState& session_state,
                                 kb::KeyBindingsEngine& keybinds,
                                 std::vector<std::unique_ptr<CanvasWindow>>& canvases,
                                 int& next_canvas_id,
                                 int& last_active_canvas_id,
                                 std::vector<ImageWindow>& images,
                                 int& next_image_id);

// Persist session state (window geometry + tool window toggles + workspace content).
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
                            const std::vector<ImageWindow>& images);

} // namespace workspace_persist


