#include "ui/preview_window.h"

#include "core/canvas.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"

#include <algorithm>
#include <cmath>

static bool PointInRect(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    return (p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y);
}

bool PreviewWindow::Render(const char* title, bool* p_open, AnsiCanvas* canvas,
                           SessionState* session, bool apply_placement_this_frame)
{
    if (!p_open || !*p_open)
        return false;

    const char* win_name = title ? title : "Preview";
    if (session)
        ApplyImGuiWindowPlacement(*session, win_name, apply_placement_this_frame);

    if (!ImGui::Begin(win_name, p_open))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, win_name);
        ImGui::End();
        return true;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, win_name);

    const AnsiCanvas::ViewState vs = canvas ? canvas->GetLastViewState() : AnsiCanvas::ViewState{};

    // Reserve drawing area.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float min_w = 220.0f;
    const float min_h = 160.0f;
    ImVec2 draw_size(std::max(min_w, avail.x), std::max(min_h, avail.y));

    ImGui::InvisibleButton("##preview_canvas", draw_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();

    // Background.
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 4.0f);

    // If we don't have a valid canvas view yet, show the empty preview area only.
    if (!canvas || !vs.valid || vs.columns <= 0 || vs.rows <= 0 || vs.canvas_w <= 0.0f || vs.canvas_h <= 0.0f)
    {
        ImGui::End();
        return true;
    }

    const float pad = 6.0f;
    ImVec2 inner0(p0.x + pad, p0.y + pad);
    ImVec2 inner1(p1.x - pad, p1.y - pad);
    if (inner1.x <= inner0.x || inner1.y <= inner0.y)
    {
        ImGui::End();
        return true;
    }

    const float inner_w = inner1.x - inner0.x;
    const float inner_h = inner1.y - inner0.y;

    // Scale full canvas into inner rect.
    //
    // We must NOT scale beyond the preview width (no horizontal cropping),
    // so we use fit-to-width scaling:
    //   scale = sx
    const float sx = inner_w / vs.canvas_w;
    const float sy = inner_h / vs.canvas_h;
    (void)sy;
    const float scale = sx;

    const float map_w = vs.canvas_w * scale;
    const float map_h = vs.canvas_h * scale;
    // Anchor minimap to the TOP of the preview area (no vertical centering),
    // so it never extends above the window when map_h > inner_h.
    const ImVec2 map0(inner0.x + (inner_w - map_w) * 0.5f,
                      inner0.y);
    const ImVec2 map1(map0.x + map_w, map0.y + map_h);

    dl->PushClipRect(inner0, inner1, true);

    // Minimap: sample the canvas into a coarse grid so it stays fast.
    const int max_grid_dim = 180;
    int grid_w = vs.columns;
    int grid_h = vs.rows;
    if (grid_w > max_grid_dim || grid_h > max_grid_dim)
    {
        if (vs.columns >= vs.rows)
        {
            grid_w = max_grid_dim;
            grid_h = std::max(1, (int)std::lround((double)vs.rows * ((double)grid_w / (double)vs.columns)));
        }
        else
        {
            grid_h = max_grid_dim;
            grid_w = std::max(1, (int)std::lround((double)vs.columns * ((double)grid_h / (double)vs.rows)));
        }
    }
    grid_w = std::max(1, grid_w);
    grid_h = std::max(1, grid_h);

    const float cell_pw = map_w / (float)grid_w;
    const float cell_ph = map_h / (float)grid_h;

    for (int gy = 0; gy < grid_h; ++gy)
    {
        const float y0 = map0.y + gy * cell_ph;
        const float y1 = y0 + cell_ph;
        const int src_row = std::clamp((int)std::floor(((gy + 0.5f) * (float)vs.rows) / (float)grid_h), 0, vs.rows - 1);

        for (int gx = 0; gx < grid_w; ++gx)
        {
            const float x0 = map0.x + gx * cell_pw;
            const float x1 = x0 + cell_pw;
            const int src_col = std::clamp((int)std::floor(((gx + 0.5f) * (float)vs.columns) / (float)grid_w), 0, vs.columns - 1);

            char32_t cp = U' ';
            AnsiCanvas::Color32 fg = 0;
            AnsiCanvas::Color32 bg = 0;
            canvas->GetCompositeCellPublic(src_row, src_col, cp, fg, bg);

            ImU32 col = 0;
            if (bg != 0)
                col = (ImU32)bg;
            else if (cp != U' ')
                col = (fg != 0) ? (ImU32)fg : IM_COL32(220, 220, 230, 255);
            else
                col = IM_COL32(14, 14, 16, 255);

            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }

    // Viewport rectangle in minimap space.
    const float vx0 = (vs.scroll_x / vs.canvas_w) * map_w;
    const float vy0 = (vs.scroll_y / vs.canvas_h) * map_h;
    const float vw  = (vs.view_w   / vs.canvas_w) * map_w;
    const float vh  = (vs.view_h   / vs.canvas_h) * map_h;

    ImVec2 rect0(map0.x + vx0, map0.y + vy0);
    ImVec2 rect1(rect0.x + vw, rect0.y + vh);

    // Clamp for sanity (can go out of bounds for tiny canvases).
    rect0.x = std::clamp(rect0.x, map0.x, map1.x);
    rect0.y = std::clamp(rect0.y, map0.y, map1.y);
    rect1.x = std::clamp(rect1.x, map0.x, map1.x);
    rect1.y = std::clamp(rect1.y, map0.y, map1.y);

    const ImU32 rect_fill = IM_COL32(255, 220, 80, 40);
    const ImU32 rect_edge = IM_COL32(255, 220, 80, 220);
    dl->AddRectFilled(rect0, rect1, rect_fill, 2.0f);
    dl->AddRect(rect0, rect1, rect_edge, 2.0f, 0, 2.0f);

    dl->PopClipRect();

    // Interaction: wheel zoom (over minimap) -> canvas zoom.
    if (hovered)
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const float factor = (wheel > 0.0f) ? 1.10f : (1.0f / 1.10f);
            canvas->SetZoom(canvas->GetZoom() * factor);
        }
    }

    // Interaction: drag viewport rectangle to pan the canvas.
    if (!hovered && !active)
        m_dragging = false;

    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        // Click-drag on rect: pan. Click elsewhere: center viewport there.
        if (PointInRect(mouse, rect0, rect1))
        {
            m_dragging = true;
            m_drag_off_x = mouse.x - rect0.x;
            m_drag_off_y = mouse.y - rect0.y;
        }
        else if (PointInRect(mouse, inner0, inner1))
        {
            const float mx = std::clamp(mouse.x, inner0.x, inner1.x);
            const float my = std::clamp(mouse.y, inner0.y, inner1.y);
            const float nx = (mx - map0.x) / map_w;
            const float ny = (my - map0.y) / map_h;
            const float target_x = nx * vs.canvas_w - vs.view_w * 0.5f;
            const float target_y = ny * vs.canvas_h - vs.view_h * 0.5f;
            canvas->RequestScrollPixels(target_x, target_y);
        }
    }

    if (m_dragging)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragging = false;
        }
        else
        {
            // New rect min in minimap space.
            float rx = mouse.x - m_drag_off_x;
            float ry = mouse.y - m_drag_off_y;
            rx = std::clamp(rx, map0.x, map1.x - (rect1.x - rect0.x));
            ry = std::clamp(ry, map0.y, map1.y - (rect1.y - rect0.y));

            const float nx = (rx - map0.x) / map_w;
            const float ny = (ry - map0.y) / map_h;

            const float target_x = nx * vs.canvas_w;
            const float target_y = ny * vs.canvas_h;
            canvas->RequestScrollPixels(target_x, target_y);
        }
    }

    ImGui::End();
    return true;
}


