#include "app/input_dispatcher.h"

#include "app/focus_router.h"
#include "core/canvas.h"
#include "core/key_bindings.h"

#include "imgui.h"

#include <SDL3/SDL.h>
#include "imgui_impl_sdl3.h"

namespace app
{

void InputDispatcher::BeginFrame(const FocusRouter& focus, bool any_popup_open)
{
    focus_ = &focus;
    any_popup_open_ = any_popup_open;
}

void InputDispatcher::DispatchEarlyFrame(const kb::EvalContext& early_ctx,
                                         kb::KeyBindingsEngine& keybinds,
                                         bool any_popup_open,
                                         bool want_text_input,
                                         bool command_palette_open,
                                         OpenCommandPaletteFn open_command_palette,
                                         void* open_command_palette_user,
                                         ClearAllCanvasFocusFn clear_all_canvas_focus,
                                         void* clear_all_canvas_focus_user) const
{
    if (any_popup_open || want_text_input || command_palette_open)
        return;
    if (!open_command_palette)
        return;

    // Robustness: some keyboard layouts/modifier states can make exact-mod chord matching
    // too strict for punctuation keys. Accept Ctrl+<semicolon key> as a fallback.
    const bool fallback_open =
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Semicolon, ImGuiInputFlags_RouteGlobal);

    if (keybinds.ActionPressed("ui.command_palette.open", early_ctx) || fallback_open)
    {
        open_command_palette(/*colour_mode=*/false, open_command_palette_user);
        if (clear_all_canvas_focus)
            clear_all_canvas_focus(clear_all_canvas_focus_user);
        return;
    }

    if (keybinds.ActionPressed("ui.colour_palette.open", early_ctx))
    {
        open_command_palette(/*colour_mode=*/true, open_command_palette_user);
        if (clear_all_canvas_focus)
            clear_all_canvas_focus(clear_all_canvas_focus_user);
        return;
    }
}

void InputDispatcher::DispatchFrame(const kb::EvalContext& early_ctx,
                                    kb::KeyBindingsEngine& keybinds,
                                    bool want_text_input,
                                    FocusNextPrevFn focus_next_prev,
                                    void* focus_next_prev_user) const
{
    if (!focus_ || any_popup_open_ || want_text_input)
        return;

    // Host-only: focus cycling depends on workspace state + canvas window IDs, so we execute it via callback.
    if (focus_next_prev)
    {
        if (keybinds.ActionPressed("ui.focus_next_canvas", early_ctx))
            focus_next_prev(+1, focus_next_prev_user);
        else if (keybinds.ActionPressed("ui.focus_prev_canvas", early_ctx))
            focus_next_prev(-1, focus_next_prev_user);
    }
}

void InputDispatcher::CollectPressedActionsForCanvas(int canvas_id,
                                                     const AnsiCanvas& canvas,
                                                     kb::KeyBindingsEngine& keybinds,
                                                     std::vector<std::string_view>& out,
                                                     std::size_t cap) const
{
    out.clear();

    if (any_popup_open_ || !focus_ || canvas_id < 0)
        return;
    if (!focus_->CanvasOwnsKeyboard(canvas_id))
        return;

    kb::EvalContext kctx;
    kctx.global = true;
    kctx.editor = true;
    kctx.canvas = true;
    kctx.selection = canvas.HasSelection();
    kctx.platform = kb::RuntimePlatform();

    keybinds.CollectPressedActions(kctx, out, cap);
}

void InputDispatcher::InjectCanvasKeyEventsForFrame(int canvas_id,
                                                    AnsiCanvas& canvas,
                                                    kb::KeyBindingsEngine& keybinds) const
{
    if (!focus_ || any_popup_open_ || canvas_id < 0)
        return;
    if (!focus_->CanvasOwnsKeyboard(canvas_id))
        return;

    AnsiCanvas::KeyEvents keys;
    kb::EvalContext kctx;
    kctx.global = true;
    kctx.editor = true;
    kctx.canvas = true;
    kctx.selection = canvas.HasSelection();
    kctx.platform = kb::RuntimePlatform();

    // While an IME composition is active, avoid injecting navigation/edit keys into the
    // tool/canvas pipeline. IME consumes these keys to edit the pre-edit string and navigate
    // candidates; double-handling would mutate the document unexpectedly.
    if (!canvas.HasImeComposition())
    {
        keys.left  = keybinds.ActionPressed("nav.caret_left", kctx);
        keys.right = keybinds.ActionPressed("nav.caret_right", kctx);
        keys.up    = keybinds.ActionPressed("nav.caret_up", kctx);
        keys.down  = keybinds.ActionPressed("nav.caret_down", kctx);
        keys.home  = keybinds.ActionPressed("nav.home", kctx);
        keys.end   = keybinds.ActionPressed("nav.end", kctx);
        keys.doc_top = keybinds.ActionPressed("nav.doc_top", kctx);
        keys.doc_bottom = keybinds.ActionPressed("nav.doc_bottom", kctx);

        keys.backspace = keybinds.ActionPressed("editor.backspace", kctx);
        if (kctx.selection)
            keys.del = keybinds.ActionPressed("selection.delete_destructive", kctx);
        else
            keys.del = keybinds.ActionPressed("editor.delete_forward_shift", kctx);
        keys.enter = keybinds.ActionPressed("editor.new_line", kctx);

        // Clipboard/selection shortcut keys:
        // Prefer ActionPressed (keybindings engine) so these participate in routing/ownership discipline.
        keys.c = keybinds.ActionPressed("edit.copy", kctx);
        keys.v = keybinds.ActionPressed("edit.paste", kctx);
        keys.x = keybinds.ActionPressed("edit.cut", kctx);
        keys.a = keybinds.ActionPressed("edit.select_all", kctx);

        // Escape is still a raw key (not a keybinding action today), but use Shortcut() to avoid
        // reintroducing non-routed IsKeyPressed probes.
        keys.escape = ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused);
    }

    canvas.SetKeyEventsForFrame(keys);
}

void InputDispatcher::CollectCanvasHotkeyActions(int canvas_id,
                                                 const AnsiCanvas& canvas,
                                                 kb::KeyBindingsEngine& keybinds,
                                                 std::vector<CanvasHotkeyAction>& out,
                                                 std::size_t cap) const
{
    out.clear();
    if (any_popup_open_ || !focus_ || canvas_id < 0)
        return;
    if (!focus_->CanvasOwnsKeyboard(canvas_id))
        return;

    std::vector<std::string_view> pressed;
    CollectPressedActionsForCanvas(canvas_id, canvas, keybinds, pressed, cap);
    if (pressed.empty())
        return;

    auto parse_positive_int = [](std::string_view s, int& out_i) -> bool {
        out_i = 0;
        if (s.empty())
            return false;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            out_i = out_i * 10 + (c - '0');
        }
        return true;
    };

    for (const std::string_view action_id : pressed)
    {
        // Hotkeys for character sets: charset.insert.f1..f12
        if (action_id.starts_with("charset.insert.f"))
        {
            int slot = 0;
            if (parse_positive_int(action_id.substr(std::string_view("charset.insert.f").size()), slot) &&
                slot >= 1 && slot <= 12)
            {
                CanvasHotkeyAction a;
                a.kind = CanvasHotkeyAction::Kind::CharsetInsertSlot;
                a.value = slot;
                out.push_back(std::move(a));
            }
            continue;
        }

        // Character set navigation (disabled by default in key-bindings due to chord conflicts).
        if (action_id == "charset.prev_set")
        {
            CanvasHotkeyAction a;
            a.kind = CanvasHotkeyAction::Kind::CharsetPrevSet;
            out.push_back(std::move(a));
            continue;
        }
        if (action_id == "charset.next_set")
        {
            CanvasHotkeyAction a;
            a.kind = CanvasHotkeyAction::Kind::CharsetNextSet;
            out.push_back(std::move(a));
            continue;
        }

        // Tool activation via keybindings: `tool.activate.<tool_id>` actions (registered by tools).
        if (action_id.starts_with("tool.activate."))
        {
            const std::string_view tid = action_id.substr(std::string_view("tool.activate.").size());
            if (!tid.empty())
            {
                CanvasHotkeyAction a;
                a.kind = CanvasHotkeyAction::Kind::ToolActivate;
                a.tool_id = std::string(tid);
                out.push_back(std::move(a));
            }
            continue;
        }

        // Tool preset slots: `tool.preset.slot.1..9` apply the Nth preset for the active tool.
        if (action_id.starts_with("tool.preset.slot."))
        {
            int d = 0;
            if (parse_positive_int(action_id.substr(std::string_view("tool.preset.slot.").size()), d) &&
                d >= 1 && d <= 9)
            {
                CanvasHotkeyAction a;
                a.kind = CanvasHotkeyAction::Kind::ToolPresetSlot;
                a.value = d;
                out.push_back(std::move(a));
            }
            continue;
        }
    }
}

bool InputDispatcher::OnSdlEventText(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_TEXT_INPUT)
    {
        // NOTE: SDL3 text input payload is UTF-8.
        const char* s = event.text.text;
        if (s && *s)
            buffered_text_input_utf8_.push_back(std::string(s));
        return true;
    }
#if defined(SDL_EVENT_TEXT_EDITING)
    if (event.type == SDL_EVENT_TEXT_EDITING)
    {
        const char* s = event.edit.text;
        if (s && *s)
        {
            BufferedTextEditingEvent te;
            te.text = std::string(s);
            te.start = event.edit.start;
            te.length = event.edit.length;
            buffered_text_editing_.push_back(std::move(te));
        }
        return true;
    }
#endif
    return false;
}

void InputDispatcher::DiscardBufferedText()
{
    buffered_text_input_utf8_.clear();
#if defined(SDL_EVENT_TEXT_EDITING)
    buffered_text_editing_.clear();
#endif
}

void InputDispatcher::FlushBufferedText(SDL_Window* window,
                                       const Target& prev_text_target,
                                       const FocusRouter& focus,
                                       FindCanvasByIdFn find_canvas_by_id,
                                       void* find_canvas_user)
{
    if (!window)
    {
        DiscardBufferedText();
        return;
    }

    // If we just stopped targeting a canvas for text input (or switched canvases),
    // cancel any active IME composition and clear the previous canvas' overlay state.
    {
        const Target cur_tt = focus.TextTarget();
        const bool prev_canvas = (prev_text_target.kind == TargetKind::CanvasGrid && prev_text_target.canvas_id >= 0);
        const bool cur_same_canvas =
            (cur_tt.kind == TargetKind::CanvasGrid && cur_tt.canvas_id >= 0 && cur_tt.canvas_id == prev_text_target.canvas_id);
        if (prev_canvas && !cur_same_canvas)
        {
            if (find_canvas_by_id)
            {
                if (AnsiCanvas* c = find_canvas_by_id(prev_text_target.canvas_id, find_canvas_user))
                    c->ClearImeComposition();
            }
            (void)SDL_ClearComposition(window);
        }
    }

    const Target tt = focus.TextTarget();

    // Flush committed text input now that we have a current-frame TextTarget.
    // - CanvasGrid target: deliver to the target canvas' typed queue immediately (same frame).
    // - Otherwise: deliver to ImGui IO directly (equivalent to backend text event processing).
    if (!buffered_text_input_utf8_.empty())
    {
        if (tt.kind == TargetKind::CanvasGrid && tt.canvas_id >= 0 && find_canvas_by_id)
        {
            for (const std::string& s : buffered_text_input_utf8_)
            {
                if (s.empty())
                    continue;
                if (AnsiCanvas* c = find_canvas_by_id(tt.canvas_id, find_canvas_user))
                {
                    // Committed text ends IME composition; clear overlay before queueing.
                    c->ClearImeComposition();
                    c->QueueTypedUtf8(s);
                }
            }
        }
        else
        {
            ImGuiIO& io = ImGui::GetIO();
            for (const std::string& s : buffered_text_input_utf8_)
            {
                if (s.empty())
                    continue;
                io.AddInputCharactersUTF8(s.c_str());
            }
        }
        buffered_text_input_utf8_.clear();
    }

#if defined(SDL_EVENT_TEXT_EDITING)
    // Flush buffered IME pre-edit events:
    // - CanvasGrid target: update canvas composition overlay state (true IME support).
    // - Otherwise: forward to the SDL backend event processor (best-effort).
    if (!buffered_text_editing_.empty())
    {
        if (tt.kind == TargetKind::CanvasGrid && tt.canvas_id >= 0 && find_canvas_by_id)
        {
            if (AnsiCanvas* c = find_canvas_by_id(tt.canvas_id, find_canvas_user))
            {
                for (const BufferedTextEditingEvent& te : buffered_text_editing_)
                    c->SetImeCompositionUtf8(te.text, te.start, te.length);
            }
        }
        else
        {
            for (const BufferedTextEditingEvent& te : buffered_text_editing_)
            {
                if (te.text.empty())
                    continue;
                SDL_Event e{};
                e.type = SDL_EVENT_TEXT_EDITING;
                e.edit.windowID = SDL_GetWindowID(window);
                e.edit.text = te.text.c_str();
                e.edit.start = te.start;
                e.edit.length = te.length;
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
        }
        buffered_text_editing_.clear();
    }
#endif
}

} // namespace app


