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

    // Only apply on the designated frame (typically first frame) so we don't
    // fight user interaction.
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(p.w, p.h), ImGuiCond_Always);
    ImGui::SetNextWindowCollapsed(p.collapsed, ImGuiCond_Always);
}

void CaptureImGuiWindowPlacement(SessionState& session, const char* window_name)
{
    if (!window_name || !*window_name)
        return;

    ImGuiWindowPlacement& p = session.imgui_windows[window_name];
    p.valid = true;

    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 sz  = ImGui::GetWindowSize();
    p.x = pos.x;
    p.y = pos.y;
    p.w = sz.x;
    p.h = sz.y;
    p.collapsed = ImGui::IsWindowCollapsed();
}


