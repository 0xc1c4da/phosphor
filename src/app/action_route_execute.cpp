#include "app/action_route_execute.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/action_execute.h"
#include "app/clipboard_utils.h"

#include "ansl/ansl_script_engine.h"

#include "core/canvas.h"
#include "core/paths.h"

#include "io/session/session_state.h"

#include "ui/tool_palette.h"

namespace app
{
namespace
{
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

static const ToolSpec* FindToolById(const ToolPalette& tp, std::string_view id)
{
    if (id.empty())
        return nullptr;
    for (const auto& t : tp.GetTools())
    {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

static bool AllowToolCommandsForAction(std::string_view action_id)
{
    // Tool commands are the only way tools can request structural canvas operations
    // (row/col shifts, inserts, etc.) without reimplementing heavy mutations in Lua.
    //
    // Important: `selection.delete_destructive` must be allowed here too. When the Select tool
    // runs as an INACTIVE fallback tool (i.e. another tool is active), it implements the
    // "delete selection" semantics by emitting tool commands for eligible full-row/full-col
    // selections; if we block commands, Delete degenerates into clear.
    return (action_id == "selection.delete_destructive") ||
           (action_id == "selection.shift_delete") ||
           (action_id == "selection.remove_row_shift_up") ||
           (action_id == "selection.remove_col_shift_left") ||
           (action_id == "selection.insert_row_shift_down") ||
           (action_id == "selection.insert_col_shift_right") ||
           // Selection transforms/crop should be available even when Select is not the active tool.
           // These are emitted as tool commands by the Select tool when ctx.out is available.
           (action_id == "selection.op.rotate_cw") ||
           (action_id == "selection.op.flip_x") ||
           (action_id == "selection.op.flip_y") ||
           (action_id == "selection.op.center") ||
           (action_id == "selection.crop");
}

static void ApplyToolCommands(AnsiCanvas& c, const std::vector<ToolCommand>& cmds)
{
    for (const ToolCommand& cmd : cmds)
    {
        switch (cmd.type)
        {
        case ToolCommand::Type::CanvasCropToSelection:
        {
            (void)c.CropToSelection();
        } break;
        case ToolCommand::Type::CanvasSelectionFlipX:
        {
            (void)c.FlipSelectionX(cmd.layer);
        } break;
        case ToolCommand::Type::CanvasSelectionFlipY:
        {
            (void)c.FlipSelectionY(cmd.layer);
        } break;
        case ToolCommand::Type::CanvasSelectionRotateCw:
        {
            (void)c.RotateSelectionCw(cmd.layer);
        } break;
        case ToolCommand::Type::CanvasSelectionCenter:
        {
            (void)c.CenterSelection(cmd.layer);
        } break;
        case ToolCommand::Type::CanvasRemoveRowShiftUp:
        {
            if (c.IsMovingSelection())
                (void)c.CommitMoveSelection();
            (void)c.RemoveRowShiftUp(cmd.y, cmd.layer);
        } break;
        case ToolCommand::Type::CanvasRemoveColShiftLeft:
        {
            if (c.IsMovingSelection())
                (void)c.CommitMoveSelection();
            (void)c.RemoveColumnShiftLeft(cmd.x, cmd.layer);
        } break;
        case ToolCommand::Type::CanvasInsertRowShiftDown:
        {
            if (c.IsMovingSelection())
                (void)c.CommitMoveSelection();
            (void)c.InsertRowShiftDown(cmd.y, cmd.layer);
        } break;
        case ToolCommand::Type::CanvasInsertColShiftRight:
        {
            if (c.IsMovingSelection())
                (void)c.CommitMoveSelection();
            (void)c.InsertColumnShiftRight(cmd.x, cmd.layer);
        } break;
        default:
            break;
        }
    }
}

static bool HostFallback(AnsiCanvas& c, std::string_view action_id)
{
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
    if (action_id == "selection.clear")
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
        int cx = 0, cy = 0;
        c.GetCaretCell(cx, cy);
        if (app::PasteSystemClipboardText(c, cx, cy))
            return true;
        return c.PasteClipboard(cx, cy);
    }
    return false;
}

static bool HostFallbackHandles(std::string_view action_id)
{
    return action_id == "edit.select_all" ||
           action_id == "selection.clear_or_cancel" ||
           action_id == "selection.clear" ||
           action_id == "edit.copy" ||
           action_id == "edit.cut" ||
           action_id == "edit.paste";
}

static bool RunToolAction(AnslScriptEngine& eng, AnsiCanvas& c, std::string_view action_id)
{
    std::vector<std::string> actions;
    actions.emplace_back(action_id);

    AnslFrameContext fctx;
    fctx.cols = c.GetColumns();
    fctx.rows = c.GetRows();
    fctx.phase = 0;
    fctx.focused = true;
    c.GetCaretCell(fctx.caret_x, fctx.caret_y);

    // Drive tool purely via actions (keyboard-only dispatch).
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
    fctx.typed = nullptr;
    fctx.cursor_valid = false;
    fctx.actions_pressed = &actions;
    fctx.allow_caret_writeback = false;

    // Allow a small set of structural selection actions to be emitted as tool commands.
    const bool allow_tool_commands = AllowToolCommandsForAction(action_id);
    ToolCommandSink sink;
    std::vector<ToolCommand> cmds;
    sink.allow_tool_commands = allow_tool_commands;
    sink.out_commands = allow_tool_commands ? &cmds : nullptr;

    std::string err;
    const bool ok = eng.RunFrame(c, c.GetActiveLayerIndex(), fctx, sink, false, err);
    (void)ok;

    if (allow_tool_commands && !cmds.empty())
        ApplyToolCommands(c, cmds);

    // Even if the tool errors, treat as handled to avoid host fallback duplication.
    return true;
}
} // namespace

bool CanExecuteRoutedActionId(std::string_view action_id,
                              const RoutedActionExecContext& ctx,
                              std::string& out_reason)
{
    out_reason.clear();

    // If host knows this action and says it cannot execute, honor that.
    if (ctx.allow_host_action_execute)
    {
        if (!app::CanExecuteActionId(action_id, ctx.host, out_reason))
            return false;
        // Note: CanExecuteActionId is intentionally conservative for unknown actions (returns true).
        // We only add additional disabling below for tool-owned actions that nobody can claim.
    }

    // If the host recognizes/handles this action, do NOT require any tool claim. This keeps
    // routing parity with `ExecuteRoutedActionId()` (which runs host actions first), and avoids
    // greying out host-handled actions like `colour.pick_attribute` in the command palette.
    if (ctx.allow_host_action_execute && app::IsHostHandledActionId(action_id))
        return true;

    // If this is a host-fallback selection/clipboard action, CanExecuteActionId already enforced the right guards.
    if (HostFallbackHandles(action_id))
        return true;

    // Tool-claimed routing requires a focused canvas; without it, tool-only actions should be disabled.
    AnsiCanvas* fc = ctx.host.focused_canvas;
    if (!fc)
    {
        out_reason = "Requires a focused canvas";
        return false;
    }

    // Active tool (compiled tool id preferred).
    const ToolSpec* active_tool = nullptr;
    if (!ctx.compiled_tool_id.empty())
        active_tool = FindToolById(ctx.tool_palette, ctx.compiled_tool_id);
    if (!active_tool)
        active_tool = ctx.tool_palette.GetActiveTool();

    // If any tool claims it, consider it executable (actual execution errors are handled at runtime).
    if (ToolClaimsAction(active_tool, action_id))
        return true;

    for (const ToolSpec& t : ctx.tool_palette.GetTools())
    {
        if (active_tool && !active_tool->id.empty() && t.id == active_tool->id)
            continue;
        if (ToolFallbackClaimsAction(t, action_id))
            return true;
    }

    // Otherwise: no known host handler, no tool claims it, no host fallback.
    out_reason = "No tool handles this action";
    return false;
}

bool ExecuteRoutedActionId(std::string_view action_id, const RoutedActionExecContext& ctx)
{
    // 1) If host knows the action and can execute it, do it first for non-canvas actions.
    // (This mirrors how the app has truly-global actions not owned by tools.)
    if (ctx.allow_host_action_execute)
    {
        if (app::ExecuteActionId(action_id, ctx.host))
            return true;
    }

    // Tool routing requires a focused canvas.
    AnsiCanvas* fc = ctx.host.focused_canvas;
    if (!fc)
        return false;

    // 2) Active tool (compiled tool id preferred).
    const ToolSpec* active_tool = nullptr;
    if (!ctx.compiled_tool_id.empty())
        active_tool = FindToolById(ctx.tool_palette, ctx.compiled_tool_id);
    if (!active_tool)
        active_tool = ctx.tool_palette.GetActiveTool();

    if (ToolClaimsAction(active_tool, action_id))
    {
        // Clipboard OS interop parity: mirror selection for copy/cut, prefer OS paste if possible.
        if (action_id == "edit.copy" || action_id == "edit.cut")
            (void)app::CopySelectionToSystemClipboardText(*fc);
        else if (action_id == "edit.paste")
        {
            int cx = 0, cy = 0;
            fc->GetCaretCell(cx, cy);
            if (app::PasteSystemClipboardText(*fc, cx, cy))
                return true;
        }

        return RunToolAction(ctx.tool_engine, *fc, action_id);
    }

    // 3) Fallback tools.
    static std::unordered_map<std::string, FallbackToolState> fallback_tools;

    auto ensure_fallback_engine = [&](const ToolSpec& t) -> AnslScriptEngine* {
        if (t.path.empty())
            return nullptr;
        FallbackToolState& st = fallback_tools[t.path];
        if (!st.engine)
        {
            st.engine = std::make_unique<AnslScriptEngine>();
            std::string err;
            if (!st.engine->Init(GetPhosphorAssetsDir(), err, &ctx.host.session_state.font_sanity_cache, false))
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
            if (!st.engine->CompileUserScript(src, fc, err))
            {
                st.last_error = err;
                return st.engine.get();
            }
            st.last_error.clear();
            st.last_source = src;
        }
        return st.engine.get();
    };

    for (const ToolSpec& t : ctx.tool_palette.GetTools())
    {
        if (active_tool && !active_tool->id.empty() && t.id == active_tool->id)
            continue;
        if (!ToolFallbackClaimsAction(t, action_id))
            continue;
        if (AnslScriptEngine* eng = ensure_fallback_engine(t))
            return RunToolAction(*eng, *fc, action_id);
        return true; // claimed but couldn't run; still treat as handled.
    }

    // 4) Host fallback for selection/clipboard actions that are not in ExecuteActionId.
    return HostFallback(*fc, action_id);
}

RoutedActionRouteResult RouteRoutedActionIdForKeybinding(std::string_view action_id, const RoutedActionExecContext& ctx)
{
    RoutedActionRouteResult rr;

    // First let host handle truly-global actions (and any host-implemented editor actions).
    if (ctx.allow_host_action_execute)
    {
        if (app::ExecuteActionId(action_id, ctx.host))
        {
            rr.handled = true;
            return rr;
        }
    }

    AnsiCanvas* fc = ctx.host.focused_canvas;
    if (!fc)
        return rr;

    const ToolSpec* active_tool = nullptr;
    if (!ctx.compiled_tool_id.empty())
        active_tool = FindToolById(ctx.tool_palette, ctx.compiled_tool_id);
    if (!active_tool)
        active_tool = ctx.tool_palette.GetActiveTool();

    if (ToolClaimsAction(active_tool, action_id))
    {
        // Clipboard OS interop parity.
        if (action_id == "edit.copy" || action_id == "edit.cut")
            (void)app::CopySelectionToSystemClipboardText(*fc);
        else if (action_id == "edit.paste")
        {
            int cx = 0, cy = 0;
            fc->GetCaretCell(cx, cy);
            if (app::PasteSystemClipboardText(*fc, cx, cy))
            {
                rr.handled = true;
                rr.request_switch_to_select_tool = true;
                return rr;
            }
        }

        rr.handled = true;
        rr.deliver_to_active_tool = true;
        return rr;
    }

    // Selection verb language ergonomics:
    // When a user invokes move/copy/stamp/place while another tool is active (typically Edit),
    // we want to switch to Select so subsequent arrow-key nudges are handled consistently.
    // (The action itself is still executed via the fallback Select tool path below.)
    if (action_id == "selection.op.move" ||
        action_id == "selection.op.copy" ||
        action_id == "selection.op.stamp" ||
        action_id == "selection.op.place")
    {
        rr.request_switch_to_select_tool = true;
    }

    // Fallback tools.
    static std::unordered_map<std::string, FallbackToolState> fallback_tools;

    auto ensure_fallback_engine = [&](const ToolSpec& t) -> AnslScriptEngine* {
        if (t.path.empty())
            return nullptr;
        FallbackToolState& st = fallback_tools[t.path];
        if (!st.engine)
        {
            st.engine = std::make_unique<AnslScriptEngine>();
            std::string err;
            if (!st.engine->Init(GetPhosphorAssetsDir(), err, &ctx.host.session_state.font_sanity_cache, false))
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
            if (!st.engine->CompileUserScript(src, fc, err))
            {
                st.last_error = err;
                return st.engine.get();
            }
            st.last_error.clear();
            st.last_source = src;
        }
        return st.engine.get();
    };

    for (const ToolSpec& t : ctx.tool_palette.GetTools())
    {
        if (active_tool && !active_tool->id.empty() && t.id == active_tool->id)
            continue;
        if (!ToolFallbackClaimsAction(t, action_id))
            continue;
        if (AnslScriptEngine* eng = ensure_fallback_engine(t))
        {
            rr.handled = RunToolAction(*eng, *fc, action_id);
            return rr;
        }
        rr.handled = true;
        return rr;
    }

    // Host fallback for selection/clipboard actions that are not in ExecuteActionId.
    rr.handled = HostFallback(*fc, action_id);
    return rr;
}
} // namespace app


