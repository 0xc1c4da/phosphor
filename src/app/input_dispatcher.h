#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kb { class KeyBindingsEngine; }
namespace kb { struct EvalContext; }

class AnsiCanvas;
struct SDL_Window;
union SDL_Event;

namespace app
{
class FocusRouter;
struct Target;

// InputDispatcher (Phase 3 scaffolding):
// Centralizes "what keys mean" queries that are still implemented via keybindings polling,
// but gates them through FocusRouter so only the current keyboard target receives intent.
//
// Transitional: we are converging Phase 3/4 by moving SDL text routing (committed + IME pre-edit)
// into this dispatcher so routing uses current-frame FocusRouter targets consistently.
class InputDispatcher
{
public:
    void BeginFrame(const FocusRouter& focus, bool any_popup_open);

    // Early-frame host keyboard policy (Phase 3/4 convergence):
    // Runs before FocusRouter::BeginFrame() and before rendering popups so "open command palette"
    // keystrokes can't leak into downstream consumers.
    using OpenCommandPaletteFn = void (*)(bool colour_mode, void* user);
    using ClearAllCanvasFocusFn = void (*)(void* user);
    void DispatchEarlyFrame(const kb::EvalContext& early_ctx,
                            kb::KeyBindingsEngine& keybinds,
                            bool any_popup_open,
                            bool want_text_input,
                            bool command_palette_open,
                            OpenCommandPaletteFn open_command_palette,
                            void* open_command_palette_user,
                            ClearAllCanvasFocusFn clear_all_canvas_focus,
                            void* clear_all_canvas_focus_user) const;

    // Dispatcher-owned host keyboard policy (Phase 3/4 convergence):
    // Provide a single ordered "keyboard policy surface" that can be expanded over time.
    // Host-only behaviors that depend on workspace/window state are executed via callbacks.
    using FocusNextPrevFn = void (*)(int dir, void* user);
    void DispatchFrame(const kb::EvalContext& early_ctx,
                       kb::KeyBindingsEngine& keybinds,
                       bool want_text_input,
                       FocusNextPrevFn focus_next_prev,
                       void* focus_next_prev_user) const;

    // Compute pressed action IDs for a specific canvas, but only when that canvas owns keyboard.
    // The action list is derived from the current ImGui IO state via the shared keybind engine.
    void CollectPressedActionsForCanvas(int canvas_id,
                                        const AnsiCanvas& canvas,
                                        kb::KeyBindingsEngine& keybinds,
                                        std::vector<std::string_view>& out,
                                        std::size_t cap = 64) const;

    // Canvas key intent synthesis (Phase 3/4 convergence):
    // Build the per-frame key snapshot for the router-selected canvas and inject it into the canvas.
    // This centralizes "what keys mean" and keeps policy consistent with SDL text routing and IME suppression.
    void InjectCanvasKeyEventsForFrame(int canvas_id,
                                       AnsiCanvas& canvas,
                                       kb::KeyBindingsEngine& keybinds) const;

    struct CanvasHotkeyAction
    {
        enum class Kind : std::uint8_t
        {
            CharsetInsertSlot,
            CharsetPrevSet,
            CharsetNextSet,
            ToolActivate,
            ToolPresetSlot,
        };

        Kind kind = Kind::CharsetInsertSlot;
        int  value = 0;           // slot/digit (1-based) for slot/digit kinds
        std::string tool_id;      // for ToolActivate
    };

    // Parse pressed-action IDs into structured "canvas hotkey" intents.
    // This keeps the string-to-int parsing and action-ID conventions centralized in one place.
    void CollectCanvasHotkeyActions(int canvas_id,
                                   const AnsiCanvas& canvas,
                                   kb::KeyBindingsEngine& keybinds,
                                   std::vector<CanvasHotkeyAction>& out,
                                   std::size_t cap = 64) const;

    // SDL text routing (Phase 4 convergence):
    // Buffer SDL_EVENT_TEXT_INPUT / SDL_EVENT_TEXT_EDITING during event polling (pre-ImGui::NewFrame),
    // then flush after FocusRouter::BeginFrame() using current-frame TextTarget.
    bool OnSdlEventText(const SDL_Event& event);
    void DiscardBufferedText();

    // `find_canvas_by_id` must return a valid canvas pointer for open canvases, or nullptr.
    using FindCanvasByIdFn = AnsiCanvas* (*)(int canvas_id, void* user);
    void FlushBufferedText(SDL_Window* window,
                           const Target& prev_text_target,
                           const FocusRouter& focus,
                           FindCanvasByIdFn find_canvas_by_id,
                           void* find_canvas_user);

private:
    const FocusRouter* focus_ = nullptr;
    bool any_popup_open_ = false;

    std::vector<std::string> buffered_text_input_utf8_;
#if defined(SDL_EVENT_TEXT_EDITING)
    struct BufferedTextEditingEvent
    {
        std::string text; // UTF-8 (SDL3 payload)
        int start = 0;
        int length = 0;
    };
    std::vector<BufferedTextEditingEvent> buffered_text_editing_;
#endif
};

} // namespace app


