#pragma once

#include <functional>
#include <string_view>

struct SDL_Window;
struct ImVec4;

struct SessionState;
class IoManager;
class SdlFileDialogQueue;
class ExportDialog;
class SettingsWindow;
class AnsiCanvas;
struct CanvasWindow;

namespace app
{
struct ActionExecContext
{
    SDL_Window* window = nullptr;

    SessionState& session_state;
    IoManager& io_manager;
    SdlFileDialogQueue& file_dialogs;
    ExportDialog& export_dialog;
    SettingsWindow& settings_window;

    AnsiCanvas* focused_canvas = nullptr;
    CanvasWindow* focused_canvas_window = nullptr;
    AnsiCanvas* active_canvas = nullptr;
    CanvasWindow* active_canvas_window = nullptr;

    bool& done;
    bool& window_fullscreen;
    bool& show_minimap_window;
    bool& show_settings_window;

    ImVec4& fg_colour;
    ImVec4& bg_colour;

    std::function<void()> create_new_canvas;
};

// Run any deferred window operations (e.g. maximize retry after leaving fullscreen).
// Should be called once per frame.
void TickDeferredWindowOps(SDL_Window* window, SessionState& session_state);

// Returns true if the action is known and can currently execute, otherwise false and fills a short disabled reason.
// This is intentionally conservative: unknown actions return true (so the palette doesn't over-disable).
bool CanExecuteActionId(std::string_view action_id, const ActionExecContext& ctx, std::string& out_reason);

// Returns true if `ExecuteActionId()` recognizes/handles this action id (even if it might be currently disabled).
// Use this for UI/routers that need to distinguish "host-handled" actions from tool-owned actions.
bool IsHostHandledActionId(std::string_view action_id);

// Execute a Phosphor action id using the same semantics as hotkeys/menus where possible.
// Returns true if the action was recognized and executed (or at least handled).
bool ExecuteActionId(std::string_view action_id, const ActionExecContext& ctx);
} // namespace app


