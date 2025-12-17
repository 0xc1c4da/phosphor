#include "io/session/imgui_persistence.h"

#include "imgui.h"

void ApplyImGuiWindowPlacement(const SessionState& session, const char* window_name, bool apply_this_frame)
{
    if (!apply_this_frame || !window_name || !*window_name)
        return;

    auto it = session.imgui_windows.find(window_name);
    if (it == session.imgui_windows.end())
        return;

    const ImGuiWindowPlacement& p = it->second;
    if (!p.valid)
        return;

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


