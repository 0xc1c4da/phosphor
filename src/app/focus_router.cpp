#include "app/focus_router.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace app
{

void FocusRouter::BeginFrame(int active_canvas_id, bool any_popup_open, const std::vector<int>& open_canvas_ids)
{
    active_canvas_id_ = active_canvas_id;
    any_popup_open_ = any_popup_open;

    // Keep the window-id mapping across frames so early-frame reconciliation (before any canvas windows
    // call NoteCanvasWindow() this frame) can still resolve NavWindow -> canvas id.
    //
    // Prune entries that point to canvases that are no longer open.
    if (!window_id_to_target_.empty())
    {
        auto is_open_canvas_id = [&](int cid) -> bool {
            for (int id : open_canvas_ids)
                if (id == cid) return true;
            return false;
        };

        for (auto it = window_id_to_target_.begin(); it != window_id_to_target_.end(); )
        {
            const Target& t = it->second;
            if (t.kind == TargetKind::CanvasGrid && !is_open_canvas_id(t.canvas_id))
                it = window_id_to_target_.erase(it);
            else
                ++it;
        }
    }
    focused_target_ = Target{};

    keyboard_override_valid_ = false;
    text_override_valid_ = false;
    keyboard_override_ = Target{};
    text_override_ = Target{};
}

void FocusRouter::NoteCanvasWindow(int canvas_id, std::uint32_t window_id, bool is_focused)
{
    if (canvas_id < 0 || window_id == 0)
        return;
    window_id_to_target_[window_id] = Target{TargetKind::CanvasGrid, canvas_id, window_id};
    if (is_focused)
        focused_target_ = Target{TargetKind::CanvasGrid, canvas_id, window_id};
}

void FocusRouter::NoteWindowTarget(TargetKind kind, std::uint32_t window_id, bool is_focused)
{
    if (window_id == 0)
        return;
    if (kind == TargetKind::None)
        return;
    // Store as a RootWindow-scoped mapping.
    window_id_to_target_[window_id] = Target{kind, -1, window_id};
    if (is_focused)
        focused_target_ = Target{kind, -1, window_id};
}

bool FocusRouter::ImGuiHasActiveTextInputWidget_() const
{
    // We need "text-widget dominance" (InputText/TextEdit owns text input when active).
    // imgui_internal gives us InputTextState which tracks the active text edit widget ID.
    ImGuiID active_id = ImGui::GetActiveID();
    if (active_id == 0)
        return false;

    ImGuiContext& g = *GImGui;
    if (g.InputTextState.ID == active_id)
        return true;

    // Fallback: if ImGui is explicitly requesting text input, assume the active widget is text.
    // This is conservative and helps cover custom text widgets that still drive WantTextInput.
    if (g.IO.WantTextInput)
        return true;

    return false;
}

Target FocusRouter::ComputeKeyboardTarget_() const
{
    if (keyboard_override_valid_)
        return keyboard_override_;

    ImGuiContext& g = *GImGui;

    // Active widget dominates keyboard.
    //
    // Important nuance for this codebase:
    // - Clicking the canvas grid uses an InvisibleButton, which sets ActiveId while held.
    // - If we treat *any* ActiveId as "ImGuiWidget", the router will never report CanvasGrid
    //   during normal canvas interaction, which breaks tool/canvas keyboard routing.
    //
    // So: when the ActiveId belongs to a canvas window (and it's not a text widget),
    // treat the keyboard target as that canvas grid.
    if (ImGui::GetActiveID() != 0)
    {
        const ImGuiID active_id = ImGui::GetActiveID();

        // If an active text widget exists, keep keyboard target as ImGuiWidget (text dominance is handled by TextTarget()).
        if (ImGuiHasActiveTextInputWidget_())
            return Target{TargetKind::ImGuiWidget, -1, (std::uint32_t)active_id};

        if (g.ActiveIdWindow)
        {
            // ActiveIdWindow is often a child window (e.g. canvas scroll child).
            // Use RootWindow so we can match the per-canvas mapping recorded by NoteCanvasWindow().
            ImGuiWindow* active_root = g.ActiveIdWindow->RootWindow ? g.ActiveIdWindow->RootWindow : g.ActiveIdWindow;
            const std::uint32_t win_id = (std::uint32_t)active_root->ID;
            auto it = window_id_to_target_.find(win_id);
            if (it != window_id_to_target_.end())
            {
                Target t = it->second;
                t.owner_id = win_id;
                if (any_popup_open_ && t.kind == TargetKind::CanvasGrid)
                    return Target{TargetKind::PopupOrModal, -1, 0};
                return t;
            }
        }

        return Target{TargetKind::ImGuiWidget, -1, (std::uint32_t)active_id};
    }

    // Otherwise, use ImGui nav window (keyboard focus) to infer target.
    if (g.NavWindow)
    {
        // NavWindow can sometimes be a child window; map via RootWindow for stable identity.
        ImGuiWindow* nav_root = g.NavWindow->RootWindow ? g.NavWindow->RootWindow : g.NavWindow;
        const std::uint32_t win_id = (std::uint32_t)nav_root->ID;
        auto it = window_id_to_target_.find(win_id);
        if (it != window_id_to_target_.end())
        {
            Target t = it->second;
            t.owner_id = win_id;
            if (any_popup_open_ && t.kind == TargetKind::CanvasGrid)
                return Target{TargetKind::PopupOrModal, -1, 0};
            return t;
        }
        return Target{TargetKind::ImGuiWidget, -1, win_id};
    }

    // Fallback: if we saw a focused target window this frame, treat it as the keyboard target.
    if (focused_target_.kind != TargetKind::None)
    {
        // Popup/modal dominance: never report CanvasGrid while any popup is open.
        if (any_popup_open_ && focused_target_.kind == TargetKind::CanvasGrid)
            return Target{TargetKind::PopupOrModal, -1, 0};
        return focused_target_;
    }

    // Popup/modal dominance: if we couldn't infer anything but a popup is open, at least report dominance.
    if (any_popup_open_)
        return Target{TargetKind::PopupOrModal, -1, 0};

    return Target{TargetKind::None, -1, 0};
}

Target FocusRouter::ComputeTextTarget_() const
{
    if (text_override_valid_)
        return text_override_;

    if (ImGuiHasActiveTextInputWidget_())
        return Target{TargetKind::ImGuiTextInput, -1, (std::uint32_t)ImGui::GetActiveID()};

    // Transitional: when no ImGui text widget is active, route text input to the canvas iff the
    // keyboard target is the canvas grid.
    const Target kb = ComputeKeyboardTarget_();
    if (!any_popup_open_ && kb.kind == TargetKind::CanvasGrid && kb.canvas_id >= 0)
        return Target{TargetKind::CanvasGrid, kb.canvas_id, kb.owner_id};

    if (any_popup_open_)
        return Target{TargetKind::PopupOrModal, -1, 0};

    return Target{TargetKind::None, -1, 0};
}

Target FocusRouter::KeyboardTarget() const
{
    return ComputeKeyboardTarget_();
}

Target FocusRouter::TextTarget() const
{
    return ComputeTextTarget_();
}

bool FocusRouter::CanvasOwnsKeyboard(int canvas_id) const
{
    const Target t = KeyboardTarget();
    return (t.kind == TargetKind::CanvasGrid && t.canvas_id == canvas_id);
}

bool FocusRouter::CanvasOwnsText(int canvas_id) const
{
    const Target t = TextTarget();
    return (t.kind == TargetKind::CanvasGrid && t.canvas_id == canvas_id);
}

void FocusRouter::FocusCanvasGrid(int canvas_id, FocusReason /*reason*/)
{
    if (canvas_id < 0)
        return;
    keyboard_override_valid_ = true;
    text_override_valid_ = true;
    keyboard_override_ = Target{TargetKind::CanvasGrid, canvas_id, 0};
    text_override_ = Target{TargetKind::CanvasGrid, canvas_id, 0};
    active_canvas_id_ = canvas_id;
    // Note: window focus requests are still handled by the host (run_frame.cpp) for now.
}

void FocusRouter::FocusWindowTarget(TargetKind kind, int canvas_id, std::uint32_t owner_id)
{
    if (kind == TargetKind::CanvasGrid)
    {
        FocusCanvasGrid(canvas_id, FocusReason::HostAction);
        return;
    }
    keyboard_override_valid_ = true;
    keyboard_override_ = Target{kind, canvas_id, owner_id};

    // Text target: if we're focusing a non-canvas surface, default to "none" unless the caller
    // explicitly uses ImGuiTextInput.
    if (kind == TargetKind::ImGuiTextInput)
    {
        text_override_valid_ = true;
        text_override_ = Target{kind, canvas_id, owner_id};
    }
    else
    {
        text_override_valid_ = true;
        text_override_ = Target{TargetKind::None, -1, 0};
    }
}

void FocusRouter::YieldToActiveCanvasGrid(FocusReason /*reason*/)
{
    if (active_canvas_id_ < 0)
        return;
    FocusCanvasGrid(active_canvas_id_, FocusReason::HostAction);
}

} // namespace app



