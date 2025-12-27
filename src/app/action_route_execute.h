#pragma once

#include <string_view>

class AnslScriptEngine;
class ToolPalette;

namespace app
{
struct ActionExecContext;

// Execute an action id using the same "tool-claimed routing" model as RunFrame:
//   active tool (handles when="active") -> fallback tools (when="inactive") -> host fallback
//
// This is intended to be used by non-keybinding action sources (e.g. command palette),
// so the action id is explicit rather than derived from key state.
struct RoutedActionExecContext
{
    const ActionExecContext& host;
    ToolPalette& tool_palette;
    AnslScriptEngine& tool_engine; // active/compiled tool engine
    std::string_view compiled_tool_id; // tool id currently compiled into tool_engine (best-effort)
    // If false, skip app::ExecuteActionId() and only consider tool claims + host fallback
    // (used by RunFrame's per-canvas keybinding routing to avoid double-executing host hotkeys).
    bool allow_host_action_execute = true;
};

// Result for the keybinding (in-frame) routing case:
// we may want to *deliver* the action to the already-running active tool engine
// rather than executing it immediately here.
struct RoutedActionRouteResult
{
    bool handled = false;
    bool deliver_to_active_tool = false;
    // Used to preserve existing UX: OS clipboard paste selects the pasted region,
    // and RunFrame switches to Select so it can be moved immediately.
    bool request_switch_to_select_tool = false;
};

// Returns true if the action can currently execute, otherwise false and fills a short disabled reason.
//
// Intended for UI sources (e.g. command palette) that want better disabled-state reasons than
// `app::CanExecuteActionId(...)` alone can provide for tool-owned actions.
bool CanExecuteRoutedActionId(std::string_view action_id,
                              const RoutedActionExecContext& ctx,
                              std::string& out_reason);

// Returns true if the action was handled (by tool or host).
bool ExecuteRoutedActionId(std::string_view action_id, const RoutedActionExecContext& ctx);

// Route an action id using the same precedence model, but:
// - If the active tool claims it, we do NOT run the tool engine here; we request delivery.
// - Fallback tools and host fallback are executed immediately.
RoutedActionRouteResult RouteRoutedActionIdForKeybinding(std::string_view action_id, const RoutedActionExecContext& ctx);
} // namespace app


