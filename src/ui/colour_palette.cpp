#include "ui/colour_palette.h"

#include <nlohmann/json.hpp>

#include "core/i18n.h"
#include "imgui.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
        ImGui::SetTooltip("%s", PHOS_TR("colour_palette.swatch_tooltip").c_str());
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

static std::string ImVec4ToHexRgb(const ImVec4& c)
{
    auto to_u8 = [](float v) -> int
    {
        v = ClampF(v, 0.0f, 1.0f);
        return (int)std::lround(v * 255.0f);
    };

    const int r = to_u8(c.x);
    const int g = to_u8(c.y);
    const int b = to_u8(c.z);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return std::string(buf);
}

static std::string TrimCopy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

static std::string MakeUniqueTitle(const nlohmann::json& j, const std::string& wanted)
{
    using nlohmann::json;
    const std::string base = TrimCopy(wanted.empty() ? std::string("Imported Palette") : wanted);

    auto title_exists = [&](const std::string& t) -> bool
    {
        if (!j.is_array())
            return false;
        for (const auto& item : j)
        {
            if (!item.is_object())
                continue;
            auto it = item.find("title");
            if (it != item.end() && it->is_string() && it->get<std::string>() == t)
                return true;
        }
        return false;
    };

    if (!title_exists(base))
        return base;

    for (int n = 2; n < 10000; ++n)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (%d)", base.c_str(), n);
        std::string cand(buf);
        if (!title_exists(cand))
            return cand;
    }
    return base; // fallback
}

bool AppendColourPaletteToJson(const char* path,
                               ColourPaletteDef def,
                               std::string& error)
{
    using nlohmann::json;
    error.clear();

    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }
    if (def.colors.empty())
    {
        error = "Palette has no colors";
        return false;
    }

    json j;
    {
        std::ifstream f(path);
        if (!f)
        {
            error = std::string("Failed to open ") + path;
            return false;
        }
        try
        {
            f >> j;
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }
    }

    if (!j.is_array())
    {
        error = "Expected top-level JSON array in color-palettes.json";
        return false;
    }

    def.title = MakeUniqueTitle(j, def.title);

    json item;
    item["title"] = def.title;
    json colors = json::array();
    for (const auto& c : def.colors)
        colors.push_back(ImVec4ToHexRgb(c));
    item["colors"] = std::move(colors);
    j.push_back(std::move(item));

    try
    {
        namespace fs = std::filesystem;
        fs::path p(path);
        fs::path tmp = p;
        tmp += ".tmp";

        {
            std::ofstream out(tmp.string());
            if (!out)
            {
                error = std::string("Failed to write ") + tmp.string();
                return false;
            }
            out << j.dump(2) << "\n";
        }

        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (ec)
        {
            // Best-effort cleanup; keep original file intact if rename fails.
            std::error_code ec2;
            fs::remove(tmp, ec2);
            error = std::string("Failed to replace ") + p.string() + ": " + ec.message();
            return false;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    return true;
}


