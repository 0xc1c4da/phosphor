#include "ui/minimap_window.h"

#include "app/canvas_preview_texture.h"
#include "core/canvas.h"

#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cmath>

static bool PointInRect(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    return (p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y);
}

bool MinimapWindow::Render(const char* title, bool* p_open, AnsiCanvas* canvas,
                           const CanvasPreviewTextureView* minimap_texture,
                           SessionState* session, bool apply_placement_this_frame)
{
    if (!p_open || !*p_open)
        return false;

    const char* win_name = title ? title : "Minimap";
    if (session)
        ApplyImGuiWindowPlacement(*session, win_name, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, win_name) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, win_name);
    if (!ImGui::Begin(win_name, p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, win_name);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return true;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, win_name);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, win_name);
        RenderImGuiWindowChromeMenu(session, win_name);
    }

    const AnsiCanvas::ViewState vs = canvas ? canvas->GetLastViewState() : AnsiCanvas::ViewState{};

    // Reserve drawing area.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float min_w = 220.0f;
    const float min_h = 160.0f;
    ImVec2 draw_size(std::max(min_w, avail.x), std::max(min_h, avail.y));

    ImGui::InvisibleButton("##minimap_canvas", draw_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();

    // Background.
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 4.0f);

    // If we don't have a valid canvas view yet, show the empty minimap area only.
    if (!canvas || !vs.valid || vs.columns <= 0 || vs.rows <= 0 || vs.canvas_w <= 0.0f || vs.canvas_h <= 0.0f)
    {
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return true;
    }

    const float pad = 6.0f;
    ImVec2 inner0(p0.x + pad, p0.y + pad);
    ImVec2 inner1(p1.x - pad, p1.y - pad);
    if (inner1.x <= inner0.x || inner1.y <= inner0.y)
    {
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return true;
    }

    const float inner_w = inner1.x - inner0.x;
    const float inner_h = inner1.y - inner0.y;

    // Scale full canvas into inner rect (fit to both width and height).
    const float sx = inner_w / vs.canvas_w;
    const float sy = inner_h / vs.canvas_h;
    const float scale = std::min(sx, sy);

    const float map_w = vs.canvas_w * scale;
    const float map_h = vs.canvas_h * scale;
    // Center minimap within the preview area.
    // Snap to pixel boundaries for crisper sampling (esp. with NEAREST sampler).
    ImVec2 map0(inner0.x + (inner_w - map_w) * 0.5f,
                inner0.y + (inner_h - map_h) * 0.5f);
    ImVec2 map1(map0.x + map_w, map0.y + map_h);
    map0.x = std::floor(map0.x);
    map0.y = std::floor(map0.y);
    map1.x = std::floor(map1.x);
    map1.y = std::floor(map1.y);

    // IMPORTANT: after snapping map0/map1 to pixel boundaries, recompute the actual drawn size.
    // Using the pre-snap floating `map_w/map_h` for coordinate transforms causes subtle mismatch
    // between the minimap image and the viewport rectangle (visible as jitter during zoom).
    const float map_w_px = std::max(1.0f, map1.x - map0.x);
    const float map_h_px = std::max(1.0f, map1.y - map0.y);

    dl->PushClipRect(inner0, inner1, true);

    // Minimap image:
    // - Prefer the Vulkan-backed texture (higher resolution + proper filtering).
    // - Fallback to a coarse sampled grid if texture isn't available.
    const bool has_tex = minimap_texture && minimap_texture->Valid();
    if (has_tex)
    {
        dl->AddImage(minimap_texture->texture_id, map0, map1, minimap_texture->uv0, minimap_texture->uv1);
    }
    else
    {
        // Fallback: sample the canvas into a coarse grid so it stays fast.
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

        const ImU32 paper = canvas->IsCanvasBackgroundWhite() ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
        const ImU32 default_fg = canvas->IsCanvasBackgroundWhite() ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

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

                ImU32 col = paper;
                if (bg != 0)
                    col = (ImU32)bg;
                else if (cp != U' ')
                    col = (fg != 0) ? (ImU32)fg : default_fg;

                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
            }
        }
    }

    // Viewport rectangle in minimap space.
    const float vx0 = (vs.scroll_x / vs.canvas_w) * map_w_px;
    const float vy0 = (vs.scroll_y / vs.canvas_h) * map_h_px;
    const float vw  = (vs.view_w   / vs.canvas_w) * map_w_px;
    const float vh  = (vs.view_h   / vs.canvas_h) * map_h_px;

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
    if (hovered && canvas && vs.valid && vs.canvas_w > 0.0f && vs.canvas_h > 0.0f)
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            // Zoom, but keep a stable "focus area" by also adjusting canvas scroll.
            //
            // - If the mouse is over the minimap content, zoom focuses that point (recenters viewport there).
            // - Otherwise, zoom focuses the current viewport center.
            //
            // IMPORTANT: match the snapping logic used by AnsiCanvas::Render() (snap based on base_cell_w).
            auto snapped_scale_for_zoom = [&](float zoom) -> float
            {
                const float base_cell_w = (vs.base_cell_w > 0.0f) ? vs.base_cell_w : 8.0f;
                float snapped_cell_w = std::floor(base_cell_w * zoom + 0.5f);
                if (snapped_cell_w < 1.0f)
                    snapped_cell_w = 1.0f;
                return (base_cell_w > 0.0f) ? (snapped_cell_w / base_cell_w) : 1.0f;
            };

            const float old_zoom = canvas->GetZoom();
            const float old_scale = snapped_scale_for_zoom(old_zoom);

            const float factor = (wheel > 0.0f) ? 1.10f : (1.0f / 1.10f);
            canvas->SetZoom(old_zoom * factor);

            const float new_zoom = canvas->GetZoom();
            const float new_scale = snapped_scale_for_zoom(new_zoom);
            const float ratio = (old_scale > 0.0f) ? (new_scale / old_scale) : 1.0f;

            // Pick focus point in old canvas pixel space.
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            float focus_world_x = vs.scroll_x + vs.view_w * 0.5f;
            float focus_world_y = vs.scroll_y + vs.view_h * 0.5f;
            if (PointInRect(mouse, map0, map1) && map_w_px > 0.0f && map_h_px > 0.0f)
            {
                const float mx = std::clamp(mouse.x, map0.x, map1.x);
                const float my = std::clamp(mouse.y, map0.y, map1.y);
                const float nx = (mx - map0.x) / map_w_px;
                const float ny = (my - map0.y) / map_h_px;
                focus_world_x = nx * vs.canvas_w;
                focus_world_y = ny * vs.canvas_h;
            }

            // Recenter viewport so the focused world point stays in view after zoom.
            const float new_scroll_x = focus_world_x * ratio - vs.view_w * 0.5f;
            const float new_scroll_y = focus_world_y * ratio - vs.view_h * 0.5f;
            canvas->RequestScrollPixels(new_scroll_x, new_scroll_y);
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
        else if (PointInRect(mouse, map0, map1))
        {
            const float mx = std::clamp(mouse.x, map0.x, map1.x);
            const float my = std::clamp(mouse.y, map0.y, map1.y);
            const float nx = (mx - map0.x) / map_w_px;
            const float ny = (my - map0.y) / map_h_px;
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

            const float nx = (rx - map0.x) / map_w_px;
            const float ny = (ry - map0.y) / map_h_px;

            const float target_x = nx * vs.canvas_w;
            const float target_y = ny * vs.canvas_h;
            canvas->RequestScrollPixels(target_x, target_y);
        }
    }

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return true;
}


