// Xterm-256 discrete color pickers (HueBar and HueWheel variants) built on top of Dear ImGui.
// The interaction is continuous in HSV/RGB space, but all rendered colors are snapped
// to the nearest xterm-256 entry so the visuals are strictly palette-based.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "colour_picker.h"

#include <cmath>
#include <cfloat>

// ------------------------------------------------------------
// Xterm-256 palette generation and helpers
// ------------------------------------------------------------

static bool   g_Xterm256Initialized = false;
static ImVec4 g_Xterm256[256];

static void InitXterm256Palette()
{
    if (g_Xterm256Initialized)
        return;

    g_Xterm256Initialized = true;

    auto set_rgb = [](int idx, int r, int g, int b)
    {
        g_Xterm256[idx] = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    };

    // 0–15: standard ANSI colors
    set_rgb(0, 0, 0, 0);
    set_rgb(1, 205, 0, 0);
    set_rgb(2, 0, 205, 0);
    set_rgb(3, 205, 205, 0);
    set_rgb(4, 0, 0, 238);
    set_rgb(5, 205, 0, 205);
    set_rgb(6, 0, 205, 205);
    set_rgb(7, 229, 229, 229);
    set_rgb(8, 127, 127, 127);
    set_rgb(9, 255, 0, 0);
    set_rgb(10, 0, 255, 0);
    set_rgb(11, 255, 255, 0);
    set_rgb(12, 92, 92, 255);
    set_rgb(13, 255, 0, 255);
    set_rgb(14, 0, 255, 255);
    set_rgb(15, 255, 255, 255);

    // 16–231: 6x6x6 color cube
    static const int level[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 16; i <= 231; ++i)
    {
        int idx = i - 16;
        int r = idx / 36;
        int g = (idx % 36) / 6;
        int b = idx % 6;
        set_rgb(i, level[r], level[g], level[b]);
    }

    // 232–255: grayscale ramp
    for (int i = 232; i <= 255; ++i)
    {
        int shade = 8 + (i - 232) * 10;
        set_rgb(i, shade, shade, shade);
    }
}

static int FindNearestXtermIndex(const ImVec4& c)
{
    InitXterm256Palette();

    int   best_idx  = 0;
    float best_dist = FLT_MAX;

    for (int i = 0; i < 256; ++i)
    {
        const ImVec4& p = g_Xterm256[i];
        float dr = c.x - p.x;
        float dg = c.y - p.y;
        float db = c.z - p.z;
        float dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist)
        {
            best_dist = dist;
            best_idx  = i;
        }
    }
    return best_idx;
}

static ImU32 ToCol32Xterm(const ImVec4& c_in)
{
    InitXterm256Palette();
    int idx = FindNearestXtermIndex(c_in);
    ImVec4 p = g_Xterm256[idx];
    p.w = c_in.w; // keep original alpha
    return ImGui::GetColorU32(p);
}

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

namespace ImGui
{

static inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float Dot(const ImVec2& a, const ImVec2& b)
{
    return a.x * b.x + a.y * b.y;
}

static void Barycentric(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& p,
                        float& out_u, float& out_v, float& out_w)
{
    ImVec2 v0 = ImVec2(b.x - a.x, b.y - a.y);
    ImVec2 v1 = ImVec2(c.x - a.x, c.y - a.y);
    ImVec2 v2 = ImVec2(p.x - a.x, p.y - a.y);

    float d00 = Dot(v0, v0);
    float d01 = Dot(v0, v1);
    float d11 = Dot(v1, v1);
    float d20 = Dot(v2, v0);
    float d21 = Dot(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    if (denom == 0.0f)
    {
        out_u = out_v = out_w = 0.0f;
        return;
    }
    float inv_denom = 1.0f / denom;
    out_v = (d11 * d20 - d01 * d21) * inv_denom;
    out_w = (d00 * d21 - d01 * d20) * inv_denom;
    out_u = 1.0f - out_v - out_w;
}

// From imgui_widgets.cpp (internal helper for color picker)
static void RenderArrowsForVerticalBar(ImDrawList* draw_list, ImVec2 pos, ImVec2 half_sz, float bar_w, float alpha)
{
    ImU32 alpha8 = IM_F32_TO_INT8_SAT(alpha);
    ImGui::RenderArrowPointingAt(draw_list, ImVec2(pos.x + half_sz.x + 1,         pos.y), ImVec2(half_sz.x + 2, half_sz.y + 1), ImGuiDir_Right, IM_COL32(0,0,0,alpha8));
    ImGui::RenderArrowPointingAt(draw_list, ImVec2(pos.x + half_sz.x,             pos.y), half_sz,                              ImGuiDir_Right, IM_COL32(255,255,255,alpha8));
    ImGui::RenderArrowPointingAt(draw_list, ImVec2(pos.x + bar_w - half_sz.x - 1, pos.y), ImVec2(half_sz.x + 2, half_sz.y + 1), ImGuiDir_Left,  IM_COL32(0,0,0,alpha8));
    ImGui::RenderArrowPointingAt(draw_list, ImVec2(pos.x + bar_w - half_sz.x,     pos.y), half_sz,                              ImGuiDir_Left,  IM_COL32(255,255,255,alpha8));
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

// Hue-bar variant: SV square + vertical hue bar + optional alpha bar.
// Returns true when col[] changed by user interaction.
bool ColorPicker4_Xterm256_HueBar(const char* label, float col[4], bool show_alpha, bool* out_used_right_click)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;

    BeginGroup();
    PushID(label);

    // Derive HSV from input RGB
    float H = 0.0f, S = 0.0f, V = 0.0f;
    ColorConvertRGBtoHSV(col[0], col[1], col[2], H, S, V);

    const float width     = CalcItemWidth();
    const float square_sz = GetFrameHeight();
    float       bars_width = square_sz; // hue/alpha bars
    float       total_bars = bars_width + style.ItemInnerSpacing.x;
    float       sv_picker_size = ImMax(1.0f, width - total_bars);

    ImVec2 picker_pos = window->DC.CursorPos;
    float bar0_pos_x = picker_pos.x + sv_picker_size + style.ItemInnerSpacing.x;
    float bar1_pos_x = bar0_pos_x + bars_width + style.ItemInnerSpacing.x;

    bool value_changed = false;

    // --- SV square interaction ---
    {
        SetCursorScreenPos(picker_pos);
        InvisibleButton("sv", ImVec2(sv_picker_size, sv_picker_size),
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (IsItemActive())
        {
            ImVec2 p = io.MousePos;
            float s = (p.x - picker_pos.x) / (sv_picker_size - 1.0f);
            float v = 1.0f - (p.y - picker_pos.y) / (sv_picker_size - 1.0f);
            S = Clamp01(s);
            V = Clamp01(v);
            value_changed = true;
        }
    }

    // --- Hue bar interaction ---
    {
        SetCursorScreenPos(ImVec2(bar0_pos_x, picker_pos.y));
        InvisibleButton("hue", ImVec2(bars_width, sv_picker_size),
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (IsItemActive())
        {
            float h = (io.MousePos.y - picker_pos.y) / (sv_picker_size - 1.0f);
            H = Clamp01(h);
            value_changed = true;
        }
    }

    // --- Alpha bar interaction (optional) ---
    if (show_alpha)
    {
        SetCursorScreenPos(ImVec2(bar1_pos_x, picker_pos.y));
        InvisibleButton("alpha", ImVec2(bars_width, sv_picker_size),
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (IsItemActive())
        {
            float a = 1.0f - (io.MousePos.y - picker_pos.y) / (sv_picker_size - 1.0f);
            col[3] = Clamp01(a);
            value_changed = true;
        }
    }

    // --- Convert HSV back to RGB for storage ---
    ColorConvertHSVtoRGB(H, S, V, col[0], col[1], col[2]);

    // Report which mouse button was used for the interaction that changed the color.
    if (value_changed && out_used_right_click)
    {
        *out_used_right_click = io.MouseDown[ImGuiMouseButton_Right] ||
                                io.MouseClicked[ImGuiMouseButton_Right];
    }

    // --- Rendering: discrete SV square ---
    ImDrawList* draw_list = window->DrawList;
    const int sv_steps = 48;
    for (int y = 0; y < sv_steps; ++y)
    {
        float v0 = 1.0f - (float)y / (float)(sv_steps - 1);
        float v1 = 1.0f - (float)(y + 1) / (float)(sv_steps - 1);
        float y0 = picker_pos.y + sv_picker_size * ((float)y / (float)sv_steps);
        float y1 = picker_pos.y + sv_picker_size * ((float)(y + 1) / (float)sv_steps);

        for (int x = 0; x < sv_steps; ++x)
        {
            float s0 = (float)x / (float)(sv_steps - 1);
            float s1 = (float)(x + 1) / (float)(sv_steps - 1);
            float x0 = picker_pos.x + sv_picker_size * ((float)x / (float)sv_steps);
            float x1 = picker_pos.x + sv_picker_size * ((float)(x + 1) / (float)sv_steps);

            float S_sample = (s0 + s1) * 0.5f;
            float V_sample = (v0 + v1) * 0.5f;

            ImVec4 c;
            ColorConvertHSVtoRGB(H, S_sample, V_sample, c.x, c.y, c.z);
            c.w = col[3] * style.Alpha;
            ImU32 col32 = ToCol32Xterm(c);
            draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col32);
        }
    }
    RenderFrameBorder(picker_pos, picker_pos + ImVec2(sv_picker_size, sv_picker_size), 0.0f);

    // Cursor crosshair
    float sv_x = picker_pos.x + Clamp01(S) * sv_picker_size;
    float sv_y = picker_pos.y + (1.0f - Clamp01(V)) * sv_picker_size;
    ImVec2 sv_cursor(sv_x, sv_y);
    float cursor_radius = sv_picker_size * 0.015f;
    draw_list->AddCircle(sv_cursor, cursor_radius * 2.0f, GetColorU32(ImVec4(0, 0, 0, 1)), 0, 1.5f);
    draw_list->AddCircle(sv_cursor, cursor_radius * 3.0f, GetColorU32(ImVec4(1, 1, 1, 1)), 0, 1.0f);

    // --- Rendering: discrete Hue bar ---
    const int hue_steps = 64;
    for (int i = 0; i < hue_steps; ++i)
    {
        float h0 = (float)i / (float)hue_steps;
        float y0 = picker_pos.y + sv_picker_size * ((float)i / (float)hue_steps);
        float y1 = picker_pos.y + sv_picker_size * ((float)(i + 1) / (float)hue_steps);

        ImVec4 c;
        ColorConvertHSVtoRGB(h0, 1.0f, 1.0f, c.x, c.y, c.z);
        c.w = style.Alpha;
        ImU32 col32 = ToCol32Xterm(c);
        draw_list->AddRectFilled(ImVec2(bar0_pos_x, y0), ImVec2(bar0_pos_x + bars_width, y1), col32);
    }
    float hue_line_y = picker_pos.y + Clamp01(H) * sv_picker_size;
    RenderFrameBorder(ImVec2(bar0_pos_x, picker_pos.y),
                      ImVec2(bar0_pos_x + bars_width, picker_pos.y + sv_picker_size), 0.0f);
        RenderArrowsForVerticalBar(draw_list,
                                   ImVec2(bar0_pos_x - 1, hue_line_y),
                                   ImVec2(bars_width * 0.3f, bars_width * 0.3f),
                                   bars_width + 2.0f,
                                   style.Alpha);

    // --- Rendering: discrete Alpha bar ---
    if (show_alpha)
    {
        ImRect bar1_bb(bar1_pos_x, picker_pos.y,
                       bar1_pos_x + bars_width, picker_pos.y + sv_picker_size);
        RenderColorRectWithAlphaCheckerboard(draw_list, bar1_bb.Min, bar1_bb.Max,
                                             0, bar1_bb.GetWidth() / 2.0f, ImVec2(0.0f, 0.0f));

        const int alpha_steps = 32;
        for (int i = 0; i < alpha_steps; ++i)
        {
            float a0 = 1.0f - (float)i / (float)alpha_steps;
            float y0 = picker_pos.y + sv_picker_size * ((float)i / (float)alpha_steps);
            float y1 = picker_pos.y + sv_picker_size * ((float)(i + 1) / (float)alpha_steps);

            ImVec4 c(col[0], col[1], col[2], a0 * style.Alpha);
            ImU32 col32 = ToCol32Xterm(c);
            draw_list->AddRectFilled(ImVec2(bar1_pos_x, y0),
                                     ImVec2(bar1_pos_x + bars_width, y1),
                                     col32);
        }

        float alpha_line_y = picker_pos.y + (1.0f - Clamp01(col[3])) * sv_picker_size;
        RenderFrameBorder(bar1_bb.Min, bar1_bb.Max, 0.0f);
        RenderArrowsForVerticalBar(draw_list,
                                   ImVec2(bar1_pos_x - 1, alpha_line_y),
                                   ImVec2(bars_width * 0.3f, bars_width * 0.3f),
                                   bars_width + 2.0f,
                                   style.Alpha);
    }

    ImGuiContext& g_ctx = *GImGui;
    PopID();
    EndGroup();

    if (value_changed && g_ctx.LastItemData.ID != 0)
        MarkItemEdited(g_ctx.LastItemData.ID);

    return value_changed;
}

// Hue-wheel variant: hue ring + SV triangle + optional alpha bar.
bool ColorPicker4_Xterm256_HueWheel(const char* label, float col[4], bool show_alpha, bool* out_used_right_click)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;

    BeginGroup();
    PushID(label);

    // Derive HSV from input RGB
    float H = 0.0f, S = 0.0f, V = 0.0f;
    ColorConvertRGBtoHSV(col[0], col[1], col[2], H, S, V);

    const float width = CalcItemWidth();
    float square_sz   = GetFrameHeight();
    float bars_width  = square_sz;
    float sv_picker_size = ImMax(1.0f, width - bars_width - g.Style.ItemInnerSpacing.x);

    ImVec2 picker_pos = window->DC.CursorPos;

    float wheel_thickness = sv_picker_size * 0.08f;
    float wheel_r_outer   = sv_picker_size * 0.50f;
    float wheel_r_inner   = wheel_r_outer - wheel_thickness;
    ImVec2 wheel_center(picker_pos.x + (sv_picker_size + bars_width) * 0.5f,
                        picker_pos.y + sv_picker_size * 0.5f);

    float triangle_r = wheel_r_inner - (int)(sv_picker_size * 0.027f);
    ImVec2 triangle_pa = ImVec2(triangle_r, 0.0f);                       // Hue point
    ImVec2 triangle_pb = ImVec2(triangle_r * -0.5f, triangle_r * -0.866025f); // Black
    ImVec2 triangle_pc = ImVec2(triangle_r * -0.5f, triangle_r * +0.866025f); // White

    bool value_changed = false;

    // --- Interaction: hue wheel + SV triangle ---
    {
        ImVec2 region_size(sv_picker_size + style.ItemInnerSpacing.x + bars_width, sv_picker_size);
        SetCursorScreenPos(picker_pos);
        InvisibleButton("hsv", region_size,
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

        if (IsItemActive())
        {
            ImVec2 p = io.MousePos;
            ImVec2 off = ImVec2(p.x - wheel_center.x, p.y - wheel_center.y);
            float dist2 = off.x * off.x + off.y * off.y;

            // Check if interacting with wheel ring
            if (dist2 >= (wheel_r_inner - 1.0f) * (wheel_r_inner - 1.0f) &&
                dist2 <= (wheel_r_outer + 1.0f) * (wheel_r_outer + 1.0f))
            {
                float angle = std::atan2(off.y, off.x);
                if (angle < 0.0f)
                    angle += 2.0f * IM_PI;
                H = angle / (2.0f * IM_PI);
                value_changed = true;
            }
            else
            {
                // Check SV triangle
                float cos_hue = std::cos(-H * 2.0f * IM_PI);
                float sin_hue = std::sin(-H * 2.0f * IM_PI);
                ImVec2 off_unrotated(
                    off.x * cos_hue - off.y * sin_hue,
                    off.x * sin_hue + off.y * cos_hue);

                float uu, vv, ww;
                Barycentric(triangle_pa, triangle_pb, triangle_pc, off_unrotated, uu, vv, ww);
                if (uu >= 0.0f && vv >= 0.0f && ww >= 0.0f)
                {
                    float V_new = Clamp01(1.0f - vv);
                    float S_new = Clamp01(uu / (V_new > 0.0001f ? V_new : 0.0001f));
                    S = S_new;
                    V = V_new;
                    value_changed = true;
                }
            }
        }
    }

    // --- Alpha bar interaction (same as HueBar variant) ---
    float bar_pos_x = picker_pos.x + sv_picker_size + style.ItemInnerSpacing.x;
    if (show_alpha)
    {
        SetCursorScreenPos(ImVec2(bar_pos_x, picker_pos.y));
        InvisibleButton("alpha", ImVec2(bars_width, sv_picker_size),
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (IsItemActive())
        {
            float a = 1.0f - (io.MousePos.y - picker_pos.y) / (sv_picker_size - 1.0f);
            col[3] = Clamp01(a);
            value_changed = true;
        }
    }

    // Convert back HSV -> RGB
    ColorConvertHSVtoRGB(H, S, V, col[0], col[1], col[2]);

    // Report which mouse button was used for the interaction that changed the color.
    if (value_changed && out_used_right_click)
    {
        *out_used_right_click = io.MouseDown[ImGuiMouseButton_Right] ||
                                io.MouseClicked[ImGuiMouseButton_Right];
    }

    ImDrawList* draw_list = window->DrawList;

    // --- Rendering: discrete hue wheel ---
    const int angle_steps = 96;
    const int radial_steps = 4;
    for (int ri = 0; ri < radial_steps; ++ri)
    {
        float r0 = wheel_r_inner + (wheel_r_outer - wheel_r_inner) * (float)ri / (float)radial_steps;
        float r1 = wheel_r_inner + (wheel_r_outer - wheel_r_inner) * (float)(ri + 1) / (float)radial_steps;

        for (int ai = 0; ai < angle_steps; ++ai)
        {
            float a0 = (2.0f * IM_PI) * (float)ai / (float)angle_steps;
            float a1 = (2.0f * IM_PI) * (float)(ai + 1) / (float)angle_steps;
            float am = (a0 + a1) * 0.5f;

            float x00 = wheel_center.x + std::cos(a0) * r0;
            float y00 = wheel_center.y + std::sin(a0) * r0;
            float x01 = wheel_center.x + std::cos(a1) * r0;
            float y01 = wheel_center.y + std::sin(a1) * r0;
            float x10 = wheel_center.x + std::cos(a0) * r1;
            float y10 = wheel_center.y + std::sin(a0) * r1;
            float x11 = wheel_center.x + std::cos(a1) * r1;
            float y11 = wheel_center.y + std::sin(a1) * r1;

            ImVec4 c;
            float h_sample = am / (2.0f * IM_PI);
            ColorConvertHSVtoRGB(h_sample, 1.0f, 1.0f, c.x, c.y, c.z);
            c.w = style.Alpha;
            ImU32 col32 = ToCol32Xterm(c);

            draw_list->AddQuadFilled(ImVec2(x00, y00), ImVec2(x01, y01),
                                     ImVec2(x11, y11), ImVec2(x10, y10), col32);
        }
    }

    // --- Rendering: SV triangle ---
    float cos_hue = std::cos(H * 2.0f * IM_PI);
    float sin_hue = std::sin(H * 2.0f * IM_PI);
    ImVec2 tra = ImVec2(wheel_center.x + triangle_pa.x * cos_hue - triangle_pa.y * sin_hue,
                        wheel_center.y + triangle_pa.x * sin_hue + triangle_pa.y * cos_hue);
    ImVec2 trb = ImVec2(wheel_center.x + triangle_pb.x * cos_hue - triangle_pb.y * sin_hue,
                        wheel_center.y + triangle_pb.x * sin_hue + triangle_pb.y * cos_hue);
    ImVec2 trc = ImVec2(wheel_center.x + triangle_pc.x * cos_hue - triangle_pc.y * sin_hue,
                        wheel_center.y + triangle_pc.x * sin_hue + triangle_pc.y * cos_hue);

    const int tri_steps = 40;
    ImVec2 tri_min(ImMin(tra.x, ImMin(trb.x, trc.x)), ImMin(tra.y, ImMin(trb.y, trc.y)));
    ImVec2 tri_max(ImMax(tra.x, ImMax(trb.x, trc.x)), ImMax(tra.y, ImMax(trb.y, trc.y)));

    float dx = (tri_max.x - tri_min.x) / (float)tri_steps;
    float dy = (tri_max.y - tri_min.y) / (float)tri_steps;

    for (int iy = 0; iy < tri_steps; ++iy)
    {
        float y0 = tri_min.y + dy * (float)iy;
        float y1 = tri_min.y + dy * (float)(iy + 1);
        for (int ix = 0; ix < tri_steps; ++ix)
        {
            float x0 = tri_min.x + dx * (float)ix;
            float x1 = tri_min.x + dx * (float)(ix + 1);
            ImVec2 p((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);

            float uu, vv, ww;
            Barycentric(tra, trb, trc, p, uu, vv, ww);
            if (uu < 0.0f || vv < 0.0f || ww < 0.0f)
                continue;

            float V_sample = Clamp01(1.0f - vv);
            float S_sample = Clamp01(uu / (V_sample > 0.0001f ? V_sample : 0.0001f));

            ImVec4 c;
            ColorConvertHSVtoRGB(H, S_sample, V_sample, c.x, c.y, c.z);
            c.w = col[3] * style.Alpha;
            ImU32 col32 = ToCol32Xterm(c);

            draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col32);
        }
    }

    // Triangle border and cursor
    draw_list->AddTriangle(tra, trb, trc, GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 1)), 1.5f);

    // Cursor inside triangle
    {
        // Convert current S,V to triangle position
        float uu = S * V;
        float vv = 1.0f - V;
        float ww = 1.0f - uu - vv;
        ImVec2 p_local = ImVec2(
            tra.x * uu + trb.x * vv + trc.x * ww,
            tra.y * uu + trb.y * vv + trc.y * ww);
        float r = wheel_thickness * 0.45f;
        draw_list->AddCircle(p_local, r * 1.2f, GetColorU32(ImVec4(0, 0, 0, 1)), 0, 1.5f);
        draw_list->AddCircle(p_local, r * 1.6f, GetColorU32(ImVec4(1, 1, 1, 1)), 0, 1.0f);
    }

    // --- Alpha bar rendering ---
    if (show_alpha)
    {
        ImRect bar_bb(bar_pos_x, picker_pos.y,
                      bar_pos_x + bars_width, picker_pos.y + sv_picker_size);
        RenderColorRectWithAlphaCheckerboard(draw_list, bar_bb.Min, bar_bb.Max,
                                             0, bar_bb.GetWidth() / 2.0f, ImVec2(0.0f, 0.0f));

        const int alpha_steps = 32;
        for (int i = 0; i < alpha_steps; ++i)
        {
            float a0 = 1.0f - (float)i / (float)alpha_steps;
            float y0 = picker_pos.y + sv_picker_size * ((float)i / (float)alpha_steps);
            float y1 = picker_pos.y + sv_picker_size * ((float)(i + 1) / (float)alpha_steps);

            ImVec4 c(col[0], col[1], col[2], a0 * style.Alpha);
            ImU32 col32 = ToCol32Xterm(c);
            draw_list->AddRectFilled(ImVec2(bar_pos_x, y0),
                                     ImVec2(bar_pos_x + bars_width, y1),
                                     col32);
        }

        float alpha_line_y = picker_pos.y + (1.0f - Clamp01(col[3])) * sv_picker_size;
        RenderFrameBorder(bar_bb.Min, bar_bb.Max, 0.0f);
        RenderArrowsForVerticalBar(draw_list,
                                   ImVec2(bar_pos_x - 1, alpha_line_y),
                                   ImVec2(bars_width * 0.3f, bars_width * 0.3f),
                                   bars_width + 2.0f,
                                   style.Alpha);
    }

    ImGuiContext& g_ctx2 = *GImGui;
    PopID();
    EndGroup();

    if (value_changed && g_ctx2.LastItemData.ID != 0)
        MarkItemEdited(g_ctx2.LastItemData.ID);

    return value_changed;
}

// -------------------------------------------------------------------------
// Composite foreground/background widget
// -------------------------------------------------------------------------

bool XtermForegroundBackgroundWidget(const char* label,
                                     ImVec4& foreground,
                                     ImVec4& background,
                                     int&   active_index)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImDrawList* draw_list = window->DrawList;
    ImGuiStyle& style = GetStyle();

    const float sz     = GetFrameHeight() * 2.0f; // side of each square
    const float offset = sz * 0.35f;              // diagonal offset
    const float pad    = 2.0f;

    ImVec2 pos = GetCursorScreenPos();

    // Reserve space for both squares.
    ImVec2 total_size = ImVec2(sz + offset + pad, sz + offset + pad);
    InvisibleButton(label, total_size);
    // Layout cursor after the reserved region (for subsequent widgets).
    ImVec2 cursor_after = GetCursorScreenPos();

    // Foreground square (top-left)
    ImVec2 fg_min = ImVec2(pos.x, pos.y);
    ImVec2 fg_max = ImVec2(fg_min.x + sz, fg_min.y + sz);

    // Background square (bottom-right, diagonally offset)
    ImVec2 bg_min = ImVec2(pos.x + offset, pos.y + offset);
    ImVec2 bg_max = ImVec2(bg_min.x + sz, bg_min.y + sz);

    // Background square (bottom layer)
    ImU32 bg_col = ToCol32Xterm(background);
    draw_list->AddRectFilled(bg_min, bg_max, bg_col, style.FrameRounding);
    draw_list->AddRect(bg_min, bg_max,
                       GetColorU32(ImVec4(1,1,1,1)),
                       style.FrameRounding, 0, 1.5f);

    // Foreground square (same size, overlapping top-right)
    ImU32 fg_col = ToCol32Xterm(foreground);
    draw_list->AddRectFilled(fg_min, fg_max, fg_col, style.FrameRounding);
    draw_list->AddRect(fg_min, fg_max,
                       GetColorU32(ImVec4(0,0,0,1)),
                       style.FrameRounding, 0, 1.5f);

    // Active highlight
    if (active_index == 0)
    {
        draw_list->AddRect(fg_min, fg_max,
                           GetColorU32(ImVec4(1,1,1,1)),
                           style.FrameRounding, 0, 2.0f);
    }
    else
    {
        draw_list->AddRect(bg_min, bg_max,
                           GetColorU32(ImVec4(1,1,0.5f,1)),
                           style.FrameRounding, 0, 2.0f);
    }

    bool changed = false;

    // Click selection: decide which square was clicked
    if (IsItemHovered() && IsMouseClicked(0))
    {
        ImVec2 m = GetIO().MousePos;
        if (m.x >= fg_min.x && m.x <= fg_max.x &&
            m.y >= fg_min.y && m.y <= fg_max.y)
        {
            active_index = 0;
            changed = true;
        }
        else if (m.x >= bg_min.x && m.x <= bg_max.x &&
                 m.y >= bg_min.y && m.y <= bg_max.y)
        {
            active_index = 1;
            changed = true;
        }
    }

    // Swap button to the right of the whole widget (so clicks don't collide with selector).
    ImVec2 swap_min = ImVec2(pos.x + sz + offset + pad * 1.5f, pos.y);
    ImVec2 swap_size(sz * 0.6f, sz * 0.6f);
    PushID(label);
    SetCursorScreenPos(swap_min);
    if (Button("##swap", swap_size))
    {
        ImVec4 tmp = foreground;
        foreground = background;
        background = tmp;
        changed = true;
    }
    PopID();

    // Draw a simple arrow over the swap button
    ImVec2 swap_max = ImVec2(swap_min.x + swap_size.x, swap_min.y + swap_size.y);
    ImVec2 c = ImVec2((swap_min.x + swap_max.x) * 0.5f,
                      (swap_min.y + swap_max.y) * 0.5f);
    float r = swap_size.x * 0.35f;
    ImU32 arrow_col = GetColorU32(ImVec4(1,1,1,1));
    draw_list->AddLine(ImVec2(c.x - r, c.y + r),
                       ImVec2(c.x + r * 0.6f, c.y - r * 0.2f),
                       arrow_col, 2.0f);
    draw_list->AddTriangleFilled(
        ImVec2(c.x + r * 0.8f, c.y - r * 0.6f),
        ImVec2(c.x + r * 1.1f, c.y - r * 0.1f),
        ImVec2(c.x + r * 0.4f, c.y - r * 0.1f),
        arrow_col);

    // Restore cursor to end of reserved area so following widgets don't overlap.
    SetCursorScreenPos(cursor_after);

    return changed;
}

} // namespace ImGui


