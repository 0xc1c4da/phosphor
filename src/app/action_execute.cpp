#include "app/action_execute.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "app/workspace.h"

#include "imgui.h"

#include "core/canvas.h"
#include "core/colour_ops.h"
#include "core/colour_system.h"

#include "io/io_manager.h"
#include "io/sdl_file_dialog_queue.h"
#include "io/session/session_state.h"

#include "ui/export_dialog.h"
#include "ui/settings.h"

namespace app
{
namespace
{
// Some window managers ignore maximize requests while the window is still in the
// fullscreen transition. We opportunistically maximize immediately, but also
// retry once SDL reports we're no longer fullscreen.
static bool g_pending_maximize_after_fullscreen_exit = false;

static void MaybeApplyPendingMaximize(SDL_Window* window, SessionState& session_state)
{
    if (!g_pending_maximize_after_fullscreen_exit)
        return;
    if (!window)
        return;

    const SDL_WindowFlags wf = SDL_GetWindowFlags(window);
    if ((wf & SDL_WINDOW_FULLSCREEN) != 0)
        return;

    // Either we're already maximized (e.g. WM applied it asynchronously), or we can request it now.
    if ((wf & SDL_WINDOW_MAXIMIZED) == 0)
        SDL_MaximizeWindow(window);

    session_state.window_maximized = true;
    g_pending_maximize_after_fullscreen_exit = false;
}

struct ColourHotkeyOps
{
    phos::colour::PaletteInstanceId pal;
    const phos::colour::Palette*    pal_def = nullptr;
    int                             pal_size = 256;
};

static ColourHotkeyOps MakeColourOpsForCanvas(const AnsiCanvas& c)
{
    auto& cs = phos::colour::GetColourSystem();
    ColourHotkeyOps ops;
    ops.pal = cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(c.GetPaletteRef()))
        ops.pal = *id;
    ops.pal_def = cs.Palettes().Get(ops.pal);
    ops.pal_size = (ops.pal_def && !ops.pal_def->rgb.empty()) ? (int)ops.pal_def->rgb.size() : 256;
    return ops;
}

static int ToPaletteIndex(const ImVec4& c, const ColourHotkeyOps& ops)
{
    auto& cs = phos::colour::GetColourSystem();
    const int r = (int)std::lround(c.x * 255.0f);
    const int g = (int)std::lround(c.y * 255.0f);
    const int b = (int)std::lround(c.z * 255.0f);
    const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();
    const std::uint8_t idx = phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(),
                                                                      ops.pal,
                                                                      (std::uint8_t)std::clamp(r, 0, 255),
                                                                      (std::uint8_t)std::clamp(g, 0, 255),
                                                                      (std::uint8_t)std::clamp(b, 0, 255),
                                                                      qp);
    return (int)std::clamp<int>((int)idx, 0, std::max(0, ops.pal_size - 1));
}

static void ApplyPaletteIndexToColour(int idx, ImVec4& dst, const ColourHotkeyOps& ops)
{
    if (!ops.pal_def || ops.pal_def->rgb.empty())
        return;
    idx = std::clamp(idx, 0, std::max(0, ops.pal_size - 1));
    const phos::colour::Rgb8 rgb = ops.pal_def->rgb[(size_t)idx];
    dst.x = (float)rgb.r / 255.0f;
    dst.y = (float)rgb.g / 255.0f;
    dst.z = (float)rgb.b / 255.0f;
    dst.w = 1.0f;
}

static bool ParseColourSetIndex(std::string_view action_id, std::string_view prefix, int& out_idx)
{
    out_idx = -1;
    if (!action_id.starts_with(prefix))
        return false;
    const std::string_view tail = action_id.substr(prefix.size());
    if (tail.empty())
        return false;
    // Parse non-negative integer suffix.
    int v = 0;
    for (char c : tail)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    out_idx = v;
    return true;
}
} // namespace

bool IsHostHandledActionId(std::string_view action_id)
{
    // NOTE: Keep this in sync with `ExecuteActionId()` below (but without performing side effects).
    // We intentionally list only actions the host *recognizes*, not actions that might be tool-owned.

    // Truly-global UI actions.
    if (action_id == "app.settings.open")
        return true;

    // File-level actions.
    if (action_id == "app.file.new" ||
        action_id == "app.file.open" ||
        action_id == "app.file.save" ||
        action_id == "app.file.save_as" ||
        action_id == "app.file.export_ansi" ||
        action_id == "app.file.export_png" ||
        action_id == "app.file.export_apng" ||
        action_id == "app.file.export_utf8" ||
        action_id == "app.file.edit_sauce" ||
        action_id == "canvas.close" ||
        action_id == "app.file.close_window" ||
        action_id == "app.quit")
    {
        return true;
    }

    // Global view/UI toggles.
    if (action_id == "view.fullscreen_toggle" ||
        action_id == "ui.toggle_preview" ||
        action_id == "ui.toggle_status_bar")
    {
        return true;
    }

    // Canvas-scoped host actions (require focused canvas; availability is checked by CanExecuteActionId()).
    if (action_id == "colour.prev_fg" ||
        action_id == "colour.next_fg" ||
        action_id == "colour.prev_bg" ||
        action_id == "colour.next_bg" ||
        action_id == "colour.default" ||
        action_id == "colour.pick_attribute" ||
        action_id == "edit.swap_fg_bg" ||
        action_id == "edit.undo" ||
        action_id == "edit.redo" ||
        action_id == "edit.select_all" ||
        action_id == "selection.clear_or_cancel" ||
        action_id == "editor.mirror_mode_toggle" ||
        action_id == "view.zoom_in" ||
        action_id == "view.zoom_out" ||
        action_id == "view.zoom_reset" ||
        action_id == "view.actual_size" ||
        action_id == "view.toggle_scroll_with_cursor" ||
        action_id == "view.scroll_up" ||
        action_id == "view.scroll_down" ||
        action_id == "view.scroll_left" ||
        action_id == "view.scroll_right")
    {
        return true;
    }

    // Direct FG/BG palette index jumps are host-handled.
    if (action_id.starts_with("colour.fg.set_") || action_id.starts_with("colour.bg.set_"))
        return true;

    return false;
}

void TickDeferredWindowOps(SDL_Window* window, SessionState& session_state)
{
    MaybeApplyPendingMaximize(window, session_state);
}

bool CanExecuteActionId(std::string_view action_id, const ActionExecContext& ctx, std::string& out_reason)
{
    out_reason.clear();

    // Actions we explicitly know require a focused canvas.
    auto needs_focused_canvas = [&](std::string_view id) -> bool {
        // Colour direct-jump actions.
        if (id.starts_with("colour.fg.set_") || id.starts_with("colour.bg.set_"))
            return true;
        return id == "edit.undo" ||
               id == "edit.redo" ||
               id == "edit.copy" ||
               id == "edit.cut" ||
               id == "edit.paste" ||
               id == "edit.select_all" ||
               id == "edit.swap_fg_bg" ||
               id == "selection.start_block" ||
               id == "selection.clear_or_cancel" ||
               id == "selection.clear" ||
               id == "selection.delete_destructive" ||
               id == "selection.shift_delete" ||
               id == "selection.remove_row_shift_up" ||
               id == "selection.remove_col_shift_left" ||
               id == "selection.insert_row_shift_down" ||
               id == "selection.insert_col_shift_right" ||
               id == "colour.prev_fg" ||
               id == "colour.next_fg" ||
               id == "colour.prev_bg" ||
               id == "colour.next_bg" ||
               id == "colour.default" ||
               id == "colour.pick_attribute" ||
               id == "editor.mirror_mode_toggle" ||
               id == "view.zoom_in" ||
               id == "view.zoom_out" ||
               id == "view.zoom_reset" ||
               id == "view.actual_size" ||
               id == "view.toggle_scroll_with_cursor" ||
               id == "view.scroll_up" ||
               id == "view.scroll_down" ||
               id == "view.scroll_left" ||
               id == "view.scroll_right" ||
               id == "ui.toggle_status_bar";
    };

    if (needs_focused_canvas(action_id))
    {
        if (!ctx.focused_canvas)
        {
            out_reason = "Requires a focused canvas";
            return false;
        }

        // Some focused-canvas actions are only meaningful when a selection exists.
        auto needs_selection = [&](std::string_view id) -> bool {
            return id == "edit.copy" ||
                   id == "edit.cut" ||
                   id == "selection.clear_or_cancel" ||
                   id == "selection.clear" ||
                   id == "selection.delete_destructive" ||
                   id == "selection.shift_delete" ||
                   id == "selection.remove_row_shift_up" ||
                   id == "selection.remove_col_shift_left" ||
                   id == "selection.insert_row_shift_down" ||
                   id == "selection.insert_col_shift_right";
        };
        if (needs_selection(action_id))
        {
            const bool has_sel = ctx.focused_canvas->HasSelection() || ctx.focused_canvas->IsMovingSelection();
            if (!has_sel)
            {
                out_reason = "Requires a selection";
                return false;
            }
        }
        return true;
    }

    // File actions that require an active canvas/project.
    auto needs_active_canvas = [&](std::string_view id) -> bool {
        return id == "app.file.save" ||
               id == "app.file.save_as" ||
               id == "app.file.export_ansi" ||
               id == "app.file.export_png" ||
               id == "app.file.export_apng" ||
               id == "app.file.export_utf8";
    };
    if (needs_active_canvas(action_id))
    {
        if (!ctx.active_canvas)
        {
            out_reason = "Requires an active canvas";
            return false;
        }
        return true;
    }

    // Otherwise: unknown or always-available.
    (void)ctx;
    return true;
}

bool ExecuteActionId(std::string_view action_id, const ActionExecContext& ctx)
{
    // Apply deferred window ops on any action execution (best-effort).
    TickDeferredWindowOps(ctx.window, ctx.session_state);

    // Truly-global UI actions.
    if (action_id == "app.settings.open")
    {
        ctx.show_settings_window = true;
        ctx.settings_window.SetOpen(true);
        return true;
    }

    // File-level actions.
    if (action_id == "app.file.new")
    {
        if (ctx.create_new_canvas)
            ctx.create_new_canvas();
        return true;
    }
    if (action_id == "app.file.open")
    {
        ctx.io_manager.RequestLoadFile(ctx.window, ctx.file_dialogs);
        return true;
    }
    if (action_id == "app.file.save")
    {
        if (ctx.active_canvas)
            ctx.io_manager.SaveProject(ctx.window, ctx.file_dialogs, ctx.active_canvas);
        return true;
    }
    if (action_id == "app.file.save_as")
    {
        if (ctx.active_canvas)
            ctx.io_manager.SaveProjectAs(ctx.window, ctx.file_dialogs, ctx.active_canvas);
        return true;
    }
    if (action_id == "app.file.export_ansi")
    {
        if (ctx.active_canvas)
            ctx.export_dialog.Open(ExportDialog::Tab::Ansi);
        return true;
    }
    if (action_id == "app.file.export_png")
    {
        if (ctx.active_canvas)
            ctx.export_dialog.Open(ExportDialog::Tab::Image);
        return true;
    }
    if (action_id == "app.file.export_apng")
    {
        if (ctx.active_canvas)
            ctx.export_dialog.Open(ExportDialog::Tab::Image);
        return true;
    }
    if (action_id == "app.file.export_utf8")
    {
        if (ctx.active_canvas)
            ctx.export_dialog.OpenPlaintextPreset(formats::plaintext::PresetId::PlainUtf8);
        return true;
    }
    if (action_id == "app.file.edit_sauce")
    {
        CanvasWindow* target = ctx.focused_canvas_window ? ctx.focused_canvas_window : ctx.active_canvas_window;
        if (target)
            target->sauce_dialog.OpenFromCanvas(target->canvas);
        return true;
    }
    if (action_id == "canvas.close")
    {
        if (ctx.focused_canvas_window)
            ctx.focused_canvas_window->open = false;
        else if (ctx.active_canvas_window)
            ctx.active_canvas_window->open = false;
        return true;
    }
    if (action_id == "app.file.close_window" || action_id == "app.quit")
    {
        ctx.done = true;
        return true;
    }

    // View/UI toggles.
    if (action_id == "view.fullscreen_toggle")
    {
        ctx.window_fullscreen = !ctx.window_fullscreen;
        const bool exiting_fullscreen = !ctx.window_fullscreen;
        if (!SDL_SetWindowFullscreen(ctx.window, ctx.window_fullscreen))
            ctx.window_fullscreen = !ctx.window_fullscreen;
        else
        {
            ctx.session_state.window_fullscreen = ctx.window_fullscreen;
            if (exiting_fullscreen)
            {
                SDL_MaximizeWindow(ctx.window);
                g_pending_maximize_after_fullscreen_exit = true;
            }
        }
        return true;
    }
    if (action_id == "ui.toggle_preview")
    {
        ctx.show_minimap_window = !ctx.show_minimap_window;
        return true;
    }
    if (action_id == "ui.toggle_status_bar")
    {
        if (ctx.focused_canvas)
            ctx.focused_canvas->ToggleStatusLineVisible();
        else if (ctx.active_canvas)
            ctx.active_canvas->ToggleStatusLineVisible();
        return true;
    }

    // Canvas-scoped (requires focused canvas).
    if (ctx.focused_canvas)
    {
        // Colour hotkeys (palette-snapped to the focused canvas palette).
        if (action_id == "colour.prev_fg" ||
            action_id == "colour.next_fg" ||
            action_id == "colour.prev_bg" ||
            action_id == "colour.next_bg" ||
            action_id == "colour.default" ||
            action_id == "colour.pick_attribute")
        {
            const ColourHotkeyOps ops = MakeColourOpsForCanvas(*ctx.focused_canvas);

            if (action_id == "colour.prev_fg")
            {
                int idx = ToPaletteIndex(ctx.fg_colour, ops);
                if (ops.pal_size > 0)
                    idx = (idx + ops.pal_size - 1) % ops.pal_size;
                ApplyPaletteIndexToColour(idx, ctx.fg_colour, ops);
                return true;
            }
            if (action_id == "colour.next_fg")
            {
                int idx = ToPaletteIndex(ctx.fg_colour, ops);
                if (ops.pal_size > 0)
                    idx = (idx + 1) % ops.pal_size;
                ApplyPaletteIndexToColour(idx, ctx.fg_colour, ops);
                return true;
            }
            if (action_id == "colour.prev_bg")
            {
                int idx = ToPaletteIndex(ctx.bg_colour, ops);
                if (ops.pal_size > 0)
                    idx = (idx + ops.pal_size - 1) % ops.pal_size;
                ApplyPaletteIndexToColour(idx, ctx.bg_colour, ops);
                return true;
            }
            if (action_id == "colour.next_bg")
            {
                int idx = ToPaletteIndex(ctx.bg_colour, ops);
                if (ops.pal_size > 0)
                    idx = (idx + 1) % ops.pal_size;
                ApplyPaletteIndexToColour(idx, ctx.bg_colour, ops);
                return true;
            }
            if (action_id == "colour.default")
            {
                ApplyPaletteIndexToColour(std::min(7, std::max(0, ops.pal_size - 1)), ctx.fg_colour, ops);
                ApplyPaletteIndexToColour(0, ctx.bg_colour, ops);
                return true;
            }
            if (action_id == "colour.pick_attribute")
            {
                int cx = 0, cy = 0;
                ctx.focused_canvas->GetCaretCell(cx, cy);
                char32_t cp = U' ';
                AnsiCanvas::ColourIndex16 fg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::ColourIndex16 bg = AnsiCanvas::kUnsetIndex16;
                if (ctx.focused_canvas->GetCompositeCellPublicIndices(cy, cx, cp, fg, bg))
                {
                    if (fg != AnsiCanvas::kUnsetIndex16) ApplyPaletteIndexToColour((int)fg, ctx.fg_colour, ops);
                    if (bg != AnsiCanvas::kUnsetIndex16) ApplyPaletteIndexToColour((int)bg, ctx.bg_colour, ops);
                }
                return true;
            }
        }

        if (action_id == "edit.swap_fg_bg")
        {
            const ImVec4 tmp = ctx.fg_colour;
            ctx.fg_colour = ctx.bg_colour;
            ctx.bg_colour = tmp;
            return true;
        }

        // Direct FG/BG palette index jumps (0..15 by default, but clamp to palette size).
        {
            const ColourHotkeyOps ops = MakeColourOpsForCanvas(*ctx.focused_canvas);
            int idx = -1;
            if (ParseColourSetIndex(action_id, "colour.fg.set_", idx))
            {
                ApplyPaletteIndexToColour(idx, ctx.fg_colour, ops);
                return true;
            }
            if (ParseColourSetIndex(action_id, "colour.bg.set_", idx))
            {
                ApplyPaletteIndexToColour(idx, ctx.bg_colour, ops);
                return true;
            }
        }

        if (action_id == "edit.undo")
        {
            ctx.focused_canvas->Undo();
            return true;
        }
        if (action_id == "edit.redo")
        {
            ctx.focused_canvas->Redo();
            return true;
        }
        if (action_id == "edit.select_all")
        {
            ctx.focused_canvas->SelectAll();
            return true;
        }
        if (action_id == "selection.clear_or_cancel")
        {
            // Match RunFrame host fallback behavior.
            if (ctx.focused_canvas->IsMovingSelection())
                (void)ctx.focused_canvas->CancelMoveSelection();
            else
                ctx.focused_canvas->ClearSelection();
            return true;
        }
        if (action_id == "editor.mirror_mode_toggle")
        {
            ctx.focused_canvas->ToggleMirrorModeEnabled();
            return true;
        }
        if (action_id == "view.zoom_in")
        {
            ctx.focused_canvas->SetZoom(ctx.focused_canvas->GetZoom() * 1.10f);
            return true;
        }
        if (action_id == "view.zoom_out")
        {
            ctx.focused_canvas->SetZoom(ctx.focused_canvas->GetZoom() / 1.10f);
            return true;
        }
        if (action_id == "view.zoom_reset" || action_id == "view.actual_size")
        {
            ctx.focused_canvas->SetZoom(1.0f);
            return true;
        }
        if (action_id == "view.toggle_scroll_with_cursor")
        {
            ctx.focused_canvas->ToggleFollowCaretEnabled();
            return true;
        }
        if (action_id == "view.scroll_up" ||
            action_id == "view.scroll_down" ||
            action_id == "view.scroll_left" ||
            action_id == "view.scroll_right")
        {
            const auto& vs = ctx.focused_canvas->GetLastViewState();
            float sx = vs.valid ? vs.scroll_x : 0.0f;
            float sy = vs.valid ? vs.scroll_y : 0.0f;
            const float step_x = (vs.valid && vs.cell_w > 0.0f) ? (vs.cell_w * 4.0f) : 64.0f;
            const float step_y = (vs.valid && vs.cell_h > 0.0f) ? (vs.cell_h * 2.0f) : 48.0f;

            if (action_id == "view.scroll_up") sy -= step_y;
            if (action_id == "view.scroll_down") sy += step_y;
            if (action_id == "view.scroll_left") sx -= step_x;
            if (action_id == "view.scroll_right") sx += step_x;
            if (sx < 0.0f) sx = 0.0f;
            if (sy < 0.0f) sy = 0.0f;
            ctx.focused_canvas->RequestScrollPixels(sx, sy);
            return true;
        }
    }

    // Colours: for now we keep this limited; palette will manage colours directly.
    (void)ctx;
    return false;
}
} // namespace app


