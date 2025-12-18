#include "ui/skin.h"

#include "imgui.h"

#include <cstring>

namespace ui
{
static bool IsNullOrEmpty(const char* s)
{
    return !s || !*s;
}

const char* DefaultThemeId()
{
    return kThemeCherry;
}

int ThemeCount()
{
    return 2;
}

const char* ThemeIdByIndex(int idx)
{
    switch (idx)
    {
        case 0: return kThemeMoonlight;
        case 1: return kThemeCherry;
        default: return kThemeMoonlight;
    }
}

const char* ThemeDisplayName(const char* theme_id)
{
    if (!theme_id) return "Moonlight";
    if (std::strcmp(theme_id, kThemeMoonlight) == 0) return "Moonlight";
    if (std::strcmp(theme_id, kThemeCherry) == 0) return "Cherry";
    return "Moonlight";
}

static void SetupStyle_Moonlight(ImGuiStyle& style)
{
    // Moonlight style by Madam-Herta from ImThemes
    style.Alpha = 1.0f;
    style.DisabledAlpha = 1.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.WindowRounding = 11.5f;
    style.WindowBorderSize = 0.0f;
    style.WindowMinSize = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(20.0f, 3.4f);
    style.FrameRounding = 11.9f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(4.3f, 5.5f);
    style.ItemInnerSpacing = ImVec2(7.1f, 1.8f);
    style.CellPadding = ImVec2(12.1f, 9.2f);
    style.IndentSpacing = 0.0f;
    style.ColumnsMinSpacing = 4.9f;
    style.ScrollbarSize = 11.6f;
    style.ScrollbarRounding = 15.9f;
    style.GrabMinSize = 3.7f;
    style.GrabRounding = 20.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.09411765f, 0.101960786f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.11372549f, 0.1254902f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.79607844f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18039216f, 0.1882353f, 0.19607843f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.14117648f, 0.16470589f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.12941177f, 0.14901961f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.1254902f, 0.27450982f, 0.57254905f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.88235295f, 0.79607844f, 0.56078434f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.95686275f, 0.95686275f, 0.95686275f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.26666668f, 0.2901961f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

static void SetupStyle_Cherry(ImGuiStyle& style)
{
    // Cherry style by r-lyeh from ImThemes
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = ImVec2(6.0f, 3.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(5.0f, 1.0f);
    style.FrameRounding = 3.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(7.0f, 1.0f);
    style.ItemInnerSpacing = ImVec2(1.0f, 1.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 6.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 13.0f;
    style.ScrollbarRounding = 16.0f;
    style.GrabMinSize = 20.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 4.0f;
    style.TabBorderSize = 1.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.88f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.28f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12941177f, 0.13725491f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.9f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.5372549f, 0.47843137f, 0.25490198f, 0.162f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.23137255f, 0.2f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.75f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.47f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.08627451f, 0.14901961f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.76f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.04f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.43f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void ApplyTheme(const char* theme_id, float ui_scale)
{
    const char* id = theme_id;
    if (IsNullOrEmpty(id))
        id = DefaultThemeId();

    if (std::strcmp(id, kThemeMoonlight) != 0 &&
        std::strcmp(id, kThemeCherry) != 0)
        id = DefaultThemeId();

    // Reset to a clean base so switching themes doesn't leak old values.
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    if (std::strcmp(id, kThemeCherry) == 0)
        SetupStyle_Cherry(style);
    else
        SetupStyle_Moonlight(style);

    // Apply current UI scale (HiDPI).
    if (ui_scale <= 0.0f)
        ui_scale = 1.0f;
    style.ScaleAllSizes(ui_scale);
    style.FontScaleDpi = ui_scale;
}
} // namespace ui


