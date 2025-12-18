// Colour Picker (Xterm-256) discrete pickers (HueBar and HueWheel variants) built on top of Dear ImGui.
// The interaction is continuous in HSV/RGB space, but all rendered colors are snapped
// to the nearest xterm-256 entry so the visuals are strictly palette-based.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/colour_picker.h"
#include "core/xterm256_palette.h"

#include <cmath>
#include <cfloat>

static inline ImVec4 SnapRgbToXterm256(const ImVec4& c_in)
{
    const int r = (int)std::lround(c_in.x * 255.0f);
    const int g = (int)std::lround(c_in.y * 255.0f);
    const int b = (int)std::lround(c_in.z * 255.0f);
    const int idx = xterm256::NearestIndex((std::uint8_t)ImClamp(r, 0, 255),
                                          (std::uint8_t)ImClamp(g, 0, 255),
                                          (std::uint8_t)ImClamp(b, 0, 255));
    const xterm256::Rgb rgb = xterm256::RgbForIndex(idx);
    ImVec4 out = c_in;
    out.x = rgb.r / 255.0f;
    out.y = rgb.g / 255.0f;
    out.z = rgb.b / 255.0f;
    return out;
}

static inline ImVec4 SnapRgbToPalette(const ImVec4& c_in, const ImVec4* palette, int palette_count)
{
    if (!palette || palette_count <= 0)
        return c_in;

    int best_i = 0;
    float best_d2 = FLT_MAX;
    for (int i = 0; i < palette_count; ++i)
    {
        const float dr = c_in.x - palette[i].x;
        const float dg = c_in.y - palette[i].y;
        const float db = c_in.z - palette[i].z;
        const float d2 = dr * dr + dg * dg + db * db;
        if (d2 < best_d2)
        {
            best_d2 = d2;
            best_i = i;
        }
    }

    ImVec4 out = palette[best_i];
    // Preserve caller alpha (our editor model is effectively RGB-only).
    out.w = c_in.w;
    return out;
}

static inline ImVec4 SnapRgbDiscrete(const ImVec4& c_in, const ImVec4* palette, int palette_count)
{
    if (palette && palette_count > 0)
        return SnapRgbToPalette(c_in, palette, palette_count);
    return SnapRgbToXterm256(c_in);
}

static inline ImU32 ToCol32DiscreteRgb(const ImVec4& c_in, float alpha_mul,
                                       const ImVec4* palette, int palette_count)
{
    // Editor colors are RGB-only. We still allow ImGui style alpha to fade UI.
    const float a = ImGui::GetStyle().Alpha * alpha_mul;
    const ImVec4 snapped = SnapRgbDiscrete(c_in, palette, palette_count);
    const int r = (int)std::lround(snapped.x * 255.0f);
    const int g = (int)std::lround(snapped.y * 255.0f);
    const int b = (int)std::lround(snapped.z * 255.0f);
    const int ai = (int)ImClamp(a * 255.0f, 0.0f, 255.0f);
    return IM_COL32((int)ImClamp(r, 0, 255),
                    (int)ImClamp(g, 0, 255),
                    (int)ImClamp(b, 0, 255),
                    ai);
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
bool ColorPicker4_Xterm256_HueBar(const char* label, float col[4], bool show_alpha, bool* out_used_right_click,
                                  float* inout_last_hue, const ImVec4* palette, int palette_count)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;

    BeginGroup();
    PushID(label);

    // Derive HSV from input RGB, but keep a persistent "cursor" HSV so the reticle
    // can move even when palette quantization keeps the resulting RGB unchanged.
    float H_from_rgb = 0.0f, S_from_rgb = 0.0f, V_from_rgb = 0.0f;
    ColorConvertRGBtoHSV(col[0], col[1], col[2], H_from_rgb, S_from_rgb, V_from_rgb);

    // Preserve hue when the color is grayscale/black (S == 0 or V == 0) because RGB->HSV
    // yields an undefined hue in that case (usually returning H=0). Without this, dragging
    // the hue bar while starting from white will "snap back" to red next frame.
    ImGuiStorage* storage = GetStateStorage();
    const ImGuiID hue_state_id = window->GetID("##last_hue");

    // Persistent HSV cursor + last RGB (detect external changes, e.g. palette button click).
    const ImGuiID hsv_h_id = window->GetID("##hsv_h");
    const ImGuiID hsv_s_id = window->GetID("##hsv_s");
    const ImGuiID hsv_v_id = window->GetID("##hsv_v");
    const ImGuiID rgb_r_id = window->GetID("##rgb_r");
    const ImGuiID rgb_g_id = window->GetID("##rgb_g");
    const ImGuiID rgb_b_id = window->GetID("##rgb_b");

    const float last_r = storage->GetFloat(rgb_r_id, col[0]);
    const float last_g = storage->GetFloat(rgb_g_id, col[1]);
    const float last_b = storage->GetFloat(rgb_b_id, col[2]);
    const bool rgb_changed_externally =
        (std::fabs(col[0] - last_r) > 1e-6f) ||
        (std::fabs(col[1] - last_g) > 1e-6f) ||
        (std::fabs(col[2] - last_b) > 1e-6f);

    float H = storage->GetFloat(hsv_h_id, H_from_rgb);
    float S = storage->GetFloat(hsv_s_id, S_from_rgb);
    float V = storage->GetFloat(hsv_v_id, V_from_rgb);

    if (rgb_changed_externally)
    {
        H = H_from_rgb;
        S = S_from_rgb;
        V = V_from_rgb;
        storage->SetFloat(hsv_h_id, H);
        storage->SetFloat(hsv_s_id, S);
        storage->SetFloat(hsv_v_id, V);
        storage->SetFloat(rgb_r_id, col[0]);
        storage->SetFloat(rgb_g_id, col[1]);
        storage->SetFloat(rgb_b_id, col[2]);
    }

    if (inout_last_hue)
    {
        if (S == 0.0f || V == 0.0f)
            H = *inout_last_hue;
        else
            *inout_last_hue = H;
    }
    else
    {
        if (S == 0.0f || V == 0.0f)
            H = storage->GetFloat(hue_state_id, H);
        else
            storage->SetFloat(hue_state_id, H);
    }

    const float width     = CalcItemWidth();
    const float square_sz = GetFrameHeight();
    float       bars_width = square_sz; // hue/alpha bars
    float       total_bars = bars_width + style.ItemInnerSpacing.x;
    float       sv_picker_size = ImMax(1.0f, width - total_bars);

    ImVec2 picker_pos = window->DC.CursorPos;
    float bar0_pos_x = picker_pos.x + sv_picker_size + style.ItemInnerSpacing.x;
    // float bar1_pos_x = bar0_pos_x + bars_width + style.ItemInnerSpacing.x; // (alpha disabled)

    bool interacted = false;

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
            interacted = true;
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
            interacted = true;
            if (inout_last_hue)
                *inout_last_hue = H;
            else
                storage->SetFloat(hue_state_id, H);
        }
    }

    // Persist cursor HSV whenever the user interacts, even if the snapped RGB doesn't change.
    if (interacted)
    {
        storage->SetFloat(hsv_h_id, H);
        storage->SetFloat(hsv_s_id, S);
        storage->SetFloat(hsv_v_id, V);
    }

    // Alpha is intentionally not part of our editor model; keep optional UI disabled by default.
    (void)show_alpha;

    // --- Convert HSV back to RGB for storage ---
    if (interacted)
    {
        if (inout_last_hue)
            *inout_last_hue = H;
        else
            storage->SetFloat(hue_state_id, H);
    }
    const float prev_r = col[0];
    const float prev_g = col[1];
    const float prev_b = col[2];
    ImVec4 rgb(col[0], col[1], col[2], col[3]);
    ColorConvertHSVtoRGB(H, S, V, rgb.x, rgb.y, rgb.z);
    const ImVec4 snapped = SnapRgbDiscrete(rgb, palette, palette_count);
    col[0] = snapped.x;
    col[1] = snapped.y;
    col[2] = snapped.z;
    // Alpha is not part of the editor model; preserve existing alpha.
    col[3] = rgb.w;

    const bool value_changed =
        (std::fabs(col[0] - prev_r) > 1e-6f) ||
        (std::fabs(col[1] - prev_g) > 1e-6f) ||
        (std::fabs(col[2] - prev_b) > 1e-6f);

    // Report which mouse button was used for the interaction that changed the color.
    if (interacted && out_used_right_click)
    {
        *out_used_right_click = io.MouseDown[ImGuiMouseButton_Right] ||
                                io.MouseClicked[ImGuiMouseButton_Right];
    }

    // Track last snapped RGB so external changes can be detected next frame.
    storage->SetFloat(rgb_r_id, col[0]);
    storage->SetFloat(rgb_g_id, col[1]);
    storage->SetFloat(rgb_b_id, col[2]);

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
            ImU32 col32 = ToCol32DiscreteRgb(c, 1.0f, palette, palette_count);
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
        ImU32 col32 = ToCol32DiscreteRgb(c, 1.0f, palette, palette_count);
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

    // No alpha bar.

    ImGuiContext& g_ctx = *GImGui;
    PopID();
    EndGroup();

    if (value_changed && g_ctx.LastItemData.ID != 0)
        MarkItemEdited(g_ctx.LastItemData.ID);

    // In palette-constrained mode, allow returning true even when the snapped color doesn't
    // change so the caller can react (e.g. keep "preview fb" tracking clicks correctly).
    const bool report_changed = (palette && palette_count > 0) ? interacted : value_changed;
    return report_changed;
}

// Hue-wheel variant: hue ring + SV triangle + optional alpha bar.
bool ColorPicker4_Xterm256_HueWheel(const char* label, float col[4], bool show_alpha, bool* out_used_right_click,
                                    float* inout_last_hue, const ImVec4* palette, int palette_count)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    ImGuiIO& io = g.IO;

    BeginGroup();
    PushID(label);

    // Derive HSV from input RGB, but keep a persistent "cursor" HSV so the reticle
    // can move even when palette quantization keeps the resulting RGB unchanged.
    float H_from_rgb = 0.0f, S_from_rgb = 0.0f, V_from_rgb = 0.0f;
    ColorConvertRGBtoHSV(col[0], col[1], col[2], H_from_rgb, S_from_rgb, V_from_rgb);

    // Same hue preservation as the hue-bar picker: keep the last hue when S==0 or V==0.
    ImGuiStorage* storage = GetStateStorage();
    const ImGuiID hue_state_id = window->GetID("##last_hue");

    // Persistent HSV cursor + last RGB (detect external changes, e.g. palette button click).
    const ImGuiID hsv_h_id = window->GetID("##hsv_h");
    const ImGuiID hsv_s_id = window->GetID("##hsv_s");
    const ImGuiID hsv_v_id = window->GetID("##hsv_v");
    const ImGuiID rgb_r_id = window->GetID("##rgb_r");
    const ImGuiID rgb_g_id = window->GetID("##rgb_g");
    const ImGuiID rgb_b_id = window->GetID("##rgb_b");

    const float last_r = storage->GetFloat(rgb_r_id, col[0]);
    const float last_g = storage->GetFloat(rgb_g_id, col[1]);
    const float last_b = storage->GetFloat(rgb_b_id, col[2]);
    const bool rgb_changed_externally =
        (std::fabs(col[0] - last_r) > 1e-6f) ||
        (std::fabs(col[1] - last_g) > 1e-6f) ||
        (std::fabs(col[2] - last_b) > 1e-6f);

    float H = storage->GetFloat(hsv_h_id, H_from_rgb);
    float S = storage->GetFloat(hsv_s_id, S_from_rgb);
    float V = storage->GetFloat(hsv_v_id, V_from_rgb);

    if (rgb_changed_externally)
    {
        H = H_from_rgb;
        S = S_from_rgb;
        V = V_from_rgb;
        storage->SetFloat(hsv_h_id, H);
        storage->SetFloat(hsv_s_id, S);
        storage->SetFloat(hsv_v_id, V);
        storage->SetFloat(rgb_r_id, col[0]);
        storage->SetFloat(rgb_g_id, col[1]);
        storage->SetFloat(rgb_b_id, col[2]);
    }

    if (inout_last_hue)
    {
        if (S == 0.0f || V == 0.0f)
            H = *inout_last_hue;
        else
            *inout_last_hue = H;
    }
    else
    {
        if (S == 0.0f || V == 0.0f)
            H = storage->GetFloat(hue_state_id, H);
        else
            storage->SetFloat(hue_state_id, H);
    }

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

    bool interacted = false;

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
                    interacted = true;
                if (inout_last_hue)
                    *inout_last_hue = H;
                else
                    storage->SetFloat(hue_state_id, H);
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
                    interacted = true;
                }
            }
        }
    }

    // Persist cursor HSV whenever the user interacts, even if the snapped RGB doesn't change.
    if (interacted)
    {
        storage->SetFloat(hsv_h_id, H);
        storage->SetFloat(hsv_s_id, S);
        storage->SetFloat(hsv_v_id, V);
    }

    // Alpha is intentionally not part of our editor model; keep optional UI disabled by default.
    float bar_pos_x = picker_pos.x + sv_picker_size + style.ItemInnerSpacing.x;
    (void)bar_pos_x;
    (void)show_alpha;

    // Convert back HSV -> RGB
    if (interacted)
    {
        if (inout_last_hue)
            *inout_last_hue = H;
        else
            storage->SetFloat(hue_state_id, H);
    }
    const float prev_r = col[0];
    const float prev_g = col[1];
    const float prev_b = col[2];
    ImVec4 rgb(col[0], col[1], col[2], col[3]);
    ColorConvertHSVtoRGB(H, S, V, rgb.x, rgb.y, rgb.z);
    const ImVec4 snapped = SnapRgbDiscrete(rgb, palette, palette_count);
    col[0] = snapped.x;
    col[1] = snapped.y;
    col[2] = snapped.z;
    col[3] = rgb.w;

    const bool value_changed =
        (std::fabs(col[0] - prev_r) > 1e-6f) ||
        (std::fabs(col[1] - prev_g) > 1e-6f) ||
        (std::fabs(col[2] - prev_b) > 1e-6f);

    // Report which mouse button was used for the interaction that changed the color.
    if (interacted && out_used_right_click)
    {
        *out_used_right_click = io.MouseDown[ImGuiMouseButton_Right] ||
                                io.MouseClicked[ImGuiMouseButton_Right];
    }

    // Track last snapped RGB so external changes can be detected next frame.
    storage->SetFloat(rgb_r_id, col[0]);
    storage->SetFloat(rgb_g_id, col[1]);
    storage->SetFloat(rgb_b_id, col[2]);

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
            ImU32 col32 = ToCol32DiscreteRgb(c, 1.0f, palette, palette_count);

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
            ImU32 col32 = ToCol32DiscreteRgb(c, 1.0f, palette, palette_count);

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

    // No alpha bar.

    ImGuiContext& g_ctx2 = *GImGui;
    PopID();
    EndGroup();

    if (value_changed && g_ctx2.LastItemData.ID != 0)
        MarkItemEdited(g_ctx2.LastItemData.ID);

    const bool report_changed = (palette && palette_count > 0) ? interacted : value_changed;
    return report_changed;
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
    ImU32 bg_col = ToCol32DiscreteRgb(background, 1.0f, nullptr, 0);
    draw_list->AddRectFilled(bg_min, bg_max, bg_col, style.FrameRounding);
    draw_list->AddRect(bg_min, bg_max,
                       GetColorU32(ImVec4(1,1,1,1)),
                       style.FrameRounding, 0, 1.5f);

    // Foreground square (same size, overlapping top-right)
    ImU32 fg_col = ToCol32DiscreteRgb(foreground, 1.0f, nullptr, 0);
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
    // Use a visible glyph for the button label, while keeping a stable ID with "##swap".
    // Note: the glyph will only render if the current ImGui font contains it.
    //
    // C++20 note: avoid u8"..." here because it yields const char8_t*, but ImGui expects const char*.
    // UTF-8 bytes for U+2B8C (⮌): E2 AE 8C
    static const char* kSwapLabel = "\xE2\xAE\x8C##swap";
    if (Button(kSwapLabel, swap_size))
    {
        ImVec4 tmp = foreground;
        foreground = background;
        background = tmp;
        changed = true;
    }
    PopID();

    // Restore cursor to end of reserved area so following widgets don't overlap.
    SetCursorScreenPos(cursor_after);

    return changed;
}

} // namespace ImGui


