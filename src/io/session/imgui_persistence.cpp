#include "io/session/imgui_persistence.h"

#include "imgui.h"

void ApplyImGuiWindowPlacement(const SessionState& session, const char* window_name, bool apply_this_frame)
{
    if (!apply_this_frame || !window_name || !*window_name)
        return;

    auto it = session.imgui_windows.find(window_name);
    if (it == session.imgui_windows.end() || !it->second.valid)
    {
        // No persisted placement yet (first time this window is created this session).
        // Provide a sane default so windows don't spawn tiny at (0,0).
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 work_pos  = vp ? vp->WorkPos  : ImVec2(0, 0);
        const ImVec2 work_size = vp ? vp->WorkSize : ImVec2(1280, 720);
        const ImVec2 center(work_pos.x + work_size.x * 0.5f,
                            work_pos.y + work_size.y * 0.5f);

        // Reasonable generic default size; specialized windows (e.g. canvases) can override
        // by calling SetNextWindowSize/Pos before this helper.
        const float margin = 40.0f;
        ImVec2 default_size(720.0f, 520.0f);
        if (default_size.x > work_size.x - margin) default_size.x = work_size.x - margin;
        if (default_size.y > work_size.y - margin) default_size.y = work_size.y - margin;
        if (default_size.x < 200.0f) default_size.x = 200.0f;
        if (default_size.y < 150.0f) default_size.y = 150.0f;

        // Avoid pivot-centering on first use: new windows can "jump" for one frame
        // while their size is being established.
        const ImVec2 top_left(center.x - default_size.x * 0.5f,
                              center.y - default_size.y * 0.5f);
        ImGui::SetNextWindowPos(top_left, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(default_size, ImGuiCond_Appearing);
        return;
    }

    const ImGuiWindowPlacement& p = it->second;

    // Clamp persisted placement to the current main viewport work rect so bad/stale session.json
    // data (e.g. from different monitor layouts, DPI changes, or earlier unstable window IDs)
    // can't spawn windows off-screen or effectively invisible.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 work_pos  = vp ? vp->WorkPos  : ImVec2(0, 0);
    const ImVec2 work_size = vp ? vp->WorkSize : ImVec2(1280, 720);
    const float margin = 20.0f;

    auto clampf = [](float v, float lo, float hi) -> float {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    };

    // Size: keep within viewport bounds (and keep a sane minimum).
    float w = p.w;
    float h = p.h;
    const float min_w = 200.0f;
    const float min_h = 150.0f;
    const float max_w = std::max(min_w, work_size.x - margin);
    const float max_h = std::max(min_h, work_size.y - margin);
    w = clampf(w, min_w, max_w);
    h = clampf(h, min_h, max_h);

    // Position: ensure at least part of the window stays inside the work rect.
    float x = p.x;
    float y = p.y;
    const float min_x = work_pos.x - (w - margin);
    const float min_y = work_pos.y - (h - margin);
    const float max_x = work_pos.x + work_size.x - margin;
    const float max_y = work_pos.y + work_size.y - margin;
    x = clampf(x, min_x, max_x);
    y = clampf(y, min_y, max_y);

    // Only apply on the designated frame (typically first frame) so we don't
    // fight user interaction.
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowCollapsed(p.collapsed, ImGuiCond_Always);
}

void CaptureImGuiWindowPlacement(SessionState& session, const char* window_name)
{
    if (!window_name || !*window_name)
        return;

    ImGuiWindowPlacement& p = session.imgui_windows[window_name];
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 sz  = ImGui::GetWindowSize();

    const bool collapsed = ImGui::IsWindowCollapsed();

    // Guard against transient / invalid sizes (common on the first Begin() of a newly-created window,
    // and can also happen during certain docking/layout transitions). If we persist these, future
    // restores can spawn windows tiny at (0,0).
    //
    // - If collapsed, allow small sizes (title bar height); we restore collapsed state separately.
    // - If not collapsed, ignore implausibly small sizes to avoid poisoning the session state.
    if (!collapsed)
    {
        constexpr float kMinW = 64.0f;
        constexpr float kMinH = 64.0f;
        if (sz.x < kMinW || sz.y < kMinH)
            return;
    }

    p.valid = true;
    p.x = pos.x;
    p.y = pos.y;
    p.w = sz.x;
    p.h = sz.y;
    p.collapsed = collapsed;
}


