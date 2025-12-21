#include "ui/colour_palette.h"

#include <nlohmann/json.hpp>

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

static inline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

ColourPaletteSwatchAction RenderColourPaletteSwatchButton(const char* label,
                                                         const ImVec4& color,
                                                         const ImVec2& size,
                                                         bool mark_foreground,
                                                         bool mark_background)
{
    ColourPaletteSwatchAction out;

    ImGuiColorEditFlags palette_button_flags =
        ImGuiColorEditFlags_NoAlpha |
        ImGuiColorEditFlags_NoPicker |
        ImGuiColorEditFlags_NoTooltip;

    // ColorButton is keyboard-navigable; Enter/Space activates it (same as left click).
    const bool activated = ImGui::ColorButton(label, color, palette_button_flags, size);

    ImGuiIO& io = ImGui::GetIO();
    if (activated)
    {
        if (io.KeyShift)
            out.set_secondary = true;
        else
            out.set_primary = true;
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        out.set_secondary = true;

    // Visual selection indicators: FG = outer outline + top-left corner triangle,
    // BG = inner outline + bottom-right corner triangle.
    if (mark_foreground || mark_background)
    {
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float rounding = ImGui::GetStyle().FrameRounding;
        const ImU32 shadow = IM_COL32(0, 0, 0, 170);
        // Requested explicit selection colours:
        // - Foreground marker: white
        // - Background marker: black
        const ImU32 fg_col = IM_COL32(255, 255, 255, 255);
        const ImU32 bg_col = IM_COL32(0, 0, 0, 255);

        if (mark_foreground)
        {
            const float t = 2.0f;
            dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f),
                        ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                        shadow, rounding, 0, t + 1.0f);
            dl->AddRect(p0, p1, fg_col, rounding, 0, t);

            const float ts = ClampF((p1.x - p0.x) * 0.45f, 8.0f, 18.0f);
            const ImVec2 a(p0.x + 1.0f, p0.y + 1.0f);
            const ImVec2 b(p0.x + 1.0f + ts, p0.y + 1.0f);
            const ImVec2 c(p0.x + 1.0f, p0.y + 1.0f + ts);
            dl->AddTriangleFilled(a, b, c, fg_col);
            // No outline stroke on the white foreground marker (per UX feedback).
        }

        if (mark_background)
        {
            const float inset = 3.5f;
            const ImVec2 q0(p0.x + inset, p0.y + inset);
            const ImVec2 q1(p1.x - inset, p1.y - inset);
            if (q1.x > q0.x + 2.0f && q1.y > q0.y + 2.0f)
            {
                const float t = 2.0f;
                dl->AddRect(ImVec2(q0.x - 1.0f, q0.y - 1.0f),
                            ImVec2(q1.x + 1.0f, q1.y + 1.0f),
                            shadow, rounding * 0.75f, 0, t + 1.0f);
                dl->AddRect(q0, q1, bg_col, rounding * 0.75f, 0, t);

                // Match foreground triangle size so the markers feel consistent.
                const float ts = ClampF((p1.x - p0.x) * 0.45f, 8.0f, 18.0f);
                const ImVec2 a(p1.x - 1.0f, p1.y - 1.0f);
                const ImVec2 b(p1.x - 1.0f - ts, p1.y - 1.0f);
                const ImVec2 c(p1.x - 1.0f, p1.y - 1.0f - ts);
                dl->AddTriangleFilled(a, b, c, bg_col);
                dl->AddTriangle(a, b, c, shadow, 1.0f);
            }
        }
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) || ImGui::IsItemFocused())
    {
        // Keep this tooltip simple; the surrounding UI can provide the full legend.
        ImGui::SetTooltip("Enter/LMB: set active (FG/BG)\nShift+Enter/RMB: set other (BG/FG)");
    }

    return out;
}

static bool HexToImVec4(const std::string& hex, ImVec4& out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;

    auto to_u8 = [](const std::string& sub) -> int
    {
        return static_cast<int>(std::strtoul(sub.c_str(), nullptr, 16));
    };

    int r = to_u8(s.substr(0, 2));
    int g = to_u8(s.substr(2, 2));
    int b = to_u8(s.substr(4, 2));
    int a = 255;
    if (s.size() == 8)
        a = to_u8(s.substr(6, 2));

    out.x = r / 255.0f;
    out.y = g / 255.0f;
    out.z = b / 255.0f;
    out.w = a / 255.0f;
    return true;
}

bool LoadColourPalettesFromJson(const char* path,
                                std::vector<ColourPaletteDef>& out,
                                std::string& error)
{
    using nlohmann::json;

    std::ifstream f(path);
    if (!f)
    {
        error = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!j.is_array())
    {
        error = "Expected top-level JSON array in color-palettes.json";
        return false;
    }

    out.clear();
    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        ColourPaletteDef def;
        if (auto it = item.find("title"); it != item.end() && it->is_string())
            def.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("colors"); it != item.end() && it->is_array())
        {
            for (const auto& c : *it)
            {
                if (!c.is_string())
                    continue;
                ImVec4 col;
                if (HexToImVec4(c.get<std::string>(), col))
                    def.colors.push_back(col);
            }
        }

        if (!def.colors.empty())
            out.push_back(std::move(def));
    }

    if (out.empty())
    {
        error = "No valid palettes found in color-palettes.json";
        return false;
    }

    error.clear();
    return true;
}


