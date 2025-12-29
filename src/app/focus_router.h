#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace app
{

// FocusRouter (Phase 1 -> Phase 2 transition):
// - Tracks the "active canvas" (document) identity.
// - Provides explicit KeyboardTarget/TextTarget queries derived from ImGui state (popup dominance,
//   text-widget dominance) to gate canvas/tools.
//
// This is a stepping stone toward the full model described in
// `references/focusmanager-refactor-document.md`.

enum class TargetKind : std::uint8_t
{
    None = 0,
    CanvasGrid,
    CharacterPaletteGrid,
    CharacterPicker,
    CharacterSets,
    LayerManager,
    BrushPalette,
    ToolPalette,
    ToolParameters,
    ToolPresets,
    Settings,
    ColourPicker,
    AnslEditor,
    Minimap,
    SixteenColoursBrowser,
    ImageWindow,
    Dialog,
    ImGuiWidget,     // ActiveId (non-text) or window-level keyboard ownership.
    ImGuiTextInput,  // ActiveId is a text-edit widget (InputText / TextEdit).
    PopupOrModal,    // Any popup/modal open (dominates all targets).
};

struct Target
{
    TargetKind kind = TargetKind::None;
    int        canvas_id = -1;       // valid when kind == CanvasGrid
    std::uint32_t owner_id = 0;      // ImGuiID-ish; meaning depends on kind
};

enum class FocusReason : std::uint8_t
{
    Unknown = 0,
    Mouse,
    Keyboard,
    PopupClose,
    CommandPaletteClose,
    HostAction,
};

class FocusRouter
{
public:
    // Call once per frame (after ImGui::NewFrame()).
    //
    // `open_canvas_ids` is used to prune stale window-id mappings from previous frames so
    // early-frame target reconciliation can still map ImGui's NavWindow -> canvas id.
    void BeginFrame(int active_canvas_id, bool any_popup_open, const std::vector<int>& open_canvas_ids);

    int ActiveCanvasId() const { return active_canvas_id_; }

    // Per-canvas note: call from inside each canvas window after ImGui::Begin().
    // This lets the router map ImGui window IDs back to canvas IDs for target reconciliation.
    void NoteCanvasWindow(int canvas_id, std::uint32_t window_id, bool is_focused);

    // Per-window note: call from inside non-canvas windows (e.g. palettes) after ImGui::Begin().
    // The router uses the root window ID to infer KeyboardTarget via ActiveIdWindow/NavWindow.
    void NoteWindowTarget(TargetKind kind, std::uint32_t window_id, bool is_focused);

    // Phase 2 targets (derived each time from current ImGui state + notes above).
    Target KeyboardTarget() const;
    Target TextTarget() const;

    bool CanvasOwnsKeyboard(int canvas_id) const;
    bool CanvasOwnsText(int canvas_id) const;

    // Explicit transitions (Phase 2 scaffolding). These are currently best-effort and are
    // intended to be wired incrementally by UI components.
    void SetActiveCanvas(int canvas_id) { active_canvas_id_ = canvas_id; }
    void FocusCanvasGrid(int canvas_id, FocusReason reason);
    void FocusWindowTarget(TargetKind kind, int canvas_id = -1, std::uint32_t owner_id = 0);
    void YieldToActiveCanvasGrid(FocusReason reason);

private:
    bool ImGuiHasActiveTextInputWidget_() const;
    Target ComputeKeyboardTarget_() const;
    Target ComputeTextTarget_() const;

    int  active_canvas_id_ = -1;
    bool any_popup_open_ = false;

    // Mapping of ImGui RootWindow ID -> app target (canvas/palette/etc).
    // This is kept across frames so early-frame reconciliation can still map NavWindow->target
    // before windows call Note*() this frame.
    std::unordered_map<std::uint32_t, Target> window_id_to_target_;
    // Keep a focused-target hint for cases where ImGui nav window is ambiguous.
    Target focused_target_;

    // Optional per-frame overrides requested by UI/host transitions.
    bool   keyboard_override_valid_ = false;
    bool   text_override_valid_ = false;
    Target keyboard_override_;
    Target text_override_;
};

} // namespace app


