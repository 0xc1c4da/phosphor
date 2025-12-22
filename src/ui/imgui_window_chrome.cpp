#include "ui/imgui_window_chrome.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

static constexpr float kMinWindowOpacity = 0.05f;

static SessionState::ImGuiWindowChromeState GetChromeStateOrDefault(const SessionState* session, const char* window_name)
{
    if (!session || !window_name || !*window_name)
        return SessionState::ImGuiWindowChromeState{};
    auto it = session->imgui_window_chrome.find(window_name);
    if (it == session->imgui_window_chrome.end())
        return SessionState::ImGuiWindowChromeState{};
    return it->second;
}

static SessionState::ImGuiWindowChromeState& GetChromeState(SessionState& session, const char* window_name)
{
    // Default construct if missing.
    return session.imgui_window_chrome[window_name ? window_name : ""];
}

ImGuiWindowFlags GetImGuiWindowChromeExtraFlags(const SessionState& session, const char* window_name)
{
    const auto st = GetChromeStateOrDefault(&session, window_name);
    const int z = std::clamp(st.z_order, 0, 2);
    if (z == 2)
    {
        // When pinned to back, prevent focus from raising it.
        return ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    return ImGuiWindowFlags_None;
}

bool PushImGuiWindowChromeAlpha(const SessionState* session, const char* window_name)
{
    const auto st = GetChromeStateOrDefault(session, window_name);
    const float opacity = std::clamp(st.opacity, kMinWindowOpacity, 1.0f);
    if (opacity == 1.0f)
        return false;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * opacity);
    return true;
}

void PopImGuiWindowChromeAlpha(bool pushed)
{
    if (pushed)
        ImGui::PopStyleVar();
}

void RenderImGuiWindowChromeMenu(SessionState* session, const char* window_name)
{
    if (!session || !window_name || !*window_name)
        return;

    // Only open the menu when right-clicking the title bar (window handle).
    bool hovered_title = false;
    if (ImGuiWindow* w = ImGui::GetCurrentWindow())
    {
        const ImRect r = w->TitleBarRect();
        hovered_title = ImGui::IsMouseHoveringRect(r.Min, r.Max, false);
    }
    if (hovered_title && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        ImGui::OpenPopup("##window_chrome");
    }

    if (!ImGui::BeginPopup("##window_chrome"))
        return;

    auto& st = GetChromeState(*session, window_name);
    st.opacity = std::clamp(st.opacity, kMinWindowOpacity, 1.0f);
    st.z_order = std::clamp(st.z_order, 0, 2);

    int z = st.z_order;
    if (ImGui::RadioButton("Z-order: Normal", z == 0)) z = 0;
    if (ImGui::RadioButton("Z-order: Pin to Front", z == 1)) z = 1;
    if (ImGui::RadioButton("Z-order: Pin to Back", z == 2)) z = 2;
    st.z_order = z;

    ImGui::Separator();

    int op = (int)std::lround(st.opacity * 100.0f);
    if (ImGui::SliderInt("Opacity", &op, 5, 100, "%d%%"))
        st.opacity = std::clamp(op / 100.0f, kMinWindowOpacity, 1.0f);

    ImGui::Separator();

    // Keep the session map small: drop default entries.
    if (st.opacity == 1.0f && st.z_order == 0)
        session->imgui_window_chrome.erase(window_name);

    ImGui::EndPopup();
}

bool RenderImGuiWindowChromeTitleBarButton(const char* id,
                                           const char* label_utf8,
                                           bool has_close_button,
                                           bool has_collapse_button,
                                           ImVec2* out_rect_min,
                                           ImVec2* out_rect_max)
{
    if (!id || !*id || !label_utf8 || !*label_utf8)
        return false;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return false;
    ImGuiContext& g = *ctx;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w)
        return false;
    if (w->Flags & ImGuiWindowFlags_NoTitleBar)
        return false;

    const ImGuiStyle& style = g.Style;
    const ImRect title = w->TitleBarRect();
    const float button_sz = ImGui::GetFrameHeight();

    // Reserve space to avoid overlapping ImGui's own title-bar buttons.
    // Keep this tight so the button sits close to the close X.
    // (ImGui already has its own internal spacing; adding more here makes it look "gappy".)
    // Note: In upstream Dear ImGui, the *collapse* button is on the LEFT of the title bar,
    // so we should NOT reserve space for it on the right. Only reserve the close 'X'.
    (void)has_collapse_button;
    constexpr float kTightGap = 1.0f;
    float reserve = 0.0f;
    if (has_close_button)
        reserve += button_sz + kTightGap;

    const ImVec2 pos(title.Max.x - style.FramePadding.x - reserve - button_sz,
                     title.Min.y + (title.GetHeight() - button_sz) * 0.5f);
    const ImVec2 size(button_sz, button_sz);

    // IMPORTANT: Title-bar widgets must NOT participate in layout sizing, otherwise
    // ImGuiWindowFlags_AlwaysAutoResize windows can "run away" (grow every frame).
    // We therefore use internal ItemAdd/ButtonBehavior WITHOUT calling ItemSize()
    // or moving the cursor.
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));

    // The current window clip rect is typically the *content* region, excluding the title bar.
    // Temporarily override it so ItemAdd/ButtonBehavior can work with a title-bar rect.
    ImGui::PushClipRect(title.Min, title.Max, /*intersect_with_current_clip_rect=*/false);

    ImGui::PushID(id);
    const ImGuiID iid = w->GetID("##titlebar_btn");

    // Manually register the item for interaction (without affecting layout sizing).
    if (!ImGui::ItemAdd(bb, iid))
    {
        if (out_rect_min) *out_rect_min = bb.Min;
        if (out_rect_max) *out_rect_max = bb.Max;
        ImGui::PopID();
        ImGui::PopClipRect();
        return false;
    }

    bool hovered = false;
    bool held = false;
    const bool clicked = ImGui::ButtonBehavior(bb, iid, &hovered, &held, ImGuiButtonFlags_None);

    if (out_rect_min) *out_rect_min = bb.Min;
    if (out_rect_max) *out_rect_max = bb.Max;

    // Draw (already clipped to title bar).
    ImDrawList* dl = w->DrawList;
    if (held || hovered)
    {
        const ImU32 bg = ImGui::GetColorU32(held ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered);
        dl->AddRectFilled(bb.Min, bb.Max, bg, /*rounding=*/3.0f);
    }
    const ImVec2 ts = ImGui::CalcTextSize(label_utf8);
    const ImVec2 tp(bb.Min.x + (size.x - ts.x) * 0.5f,
                    bb.Min.y + (size.y - ts.y) * 0.5f);
    ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!hovered && !held)
        text_col.w *= 0.75f;
    dl->AddText(tp, ImGui::GetColorU32(text_col), label_utf8);

    ImGui::PopID();
    ImGui::PopClipRect();
    return clicked;
}

void ApplyImGuiWindowChromeZOrder(const SessionState* session, const char* window_name)
{
    if (!session || !window_name || !*window_name)
        return;

    const auto st = GetChromeStateOrDefault(session, window_name);
    const int z = std::clamp(st.z_order, 0, 2);
    if (z == 0)
        return;

    if (ImGuiWindow* w = ImGui::FindWindowByName(window_name))
    {
        if (z == 1)
            ImGui::BringWindowToDisplayFront(w);
        else if (z == 2)
            ImGui::BringWindowToDisplayBack(w);
    }
}

void ApplyImGuiWindowChromeGlobalZOrder(const SessionState& session)
{
    // Important UX rule: popups/tooltips must remain above pinned windows,
    // otherwise the chrome menu itself can become unreachable.
    std::vector<ImGuiWindow*> overlays;
    if (ImGuiContext* ctx = ImGui::GetCurrentContext())
    {
        ImGuiContext& g = *ctx;
        overlays.reserve((size_t)g.Windows.Size);
        for (int i = 0; i < g.Windows.Size; ++i)
        {
            ImGuiWindow* w = g.Windows[i];
            if (!w)
                continue;
            const ImGuiWindowFlags wf = w->Flags;
            if (wf & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_Tooltip))
                overlays.push_back(w);
        }
    }

    std::vector<std::string> back;
    std::vector<std::string> front;
    back.reserve(session.imgui_window_chrome.size());
    front.reserve(session.imgui_window_chrome.size());

    for (const auto& kv : session.imgui_window_chrome)
    {
        const int z = std::clamp(kv.second.z_order, 0, 2);
        if (z == 2)
            back.push_back(kv.first);
        else if (z == 1)
            front.push_back(kv.first);
    }

    // Deterministic ordering (map is unordered).
    std::sort(back.begin(), back.end());
    std::sort(front.begin(), front.end());

    // First push "back" windows to the back of the display stack.
    for (const auto& name : back)
    {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name.c_str()))
            ImGui::BringWindowToDisplayBack(w);
    }

    // Then re-assert "front" windows as a final pass so they win over any focus-induced raising.
    for (const auto& name : front)
    {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name.c_str()))
            ImGui::BringWindowToDisplayFront(w);
    }

    // Finally, re-assert overlays on top.
    for (ImGuiWindow* w : overlays)
        ImGui::BringWindowToDisplayFront(w);
}


