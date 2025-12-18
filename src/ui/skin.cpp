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
    return 3;
}

const char* ThemeIdByIndex(int idx)
{
    switch (idx)
    {
        case 0: return kThemeCherry;
        case 1: return kThemeGrape;
        case 2: return kThemeCharcoal;
        default: return DefaultThemeId();
    }
}

const char* ThemeDisplayName(const char* theme_id)
{
    if (IsNullOrEmpty(theme_id)) theme_id = DefaultThemeId();
    if (std::strcmp(theme_id, kThemeCherry) == 0) return "Cherry";
    if (std::strcmp(theme_id, kThemeGrape) == 0) return "Grape";
    if (std::strcmp(theme_id, kThemeCharcoal) == 0) return "Charcoal";
    return ThemeDisplayName(DefaultThemeId());
}

static bool IsKnownThemeId(const char* theme_id)
{
    if (IsNullOrEmpty(theme_id))
        return false;
    return std::strcmp(theme_id, kThemeCherry) == 0 ||
           std::strcmp(theme_id, kThemeGrape) == 0 ||
           std::strcmp(theme_id, kThemeCharcoal) == 0;
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

static void SetupStyle_QuickMinimalLook(ImGuiStyle& style)
{
    // Quick minimal look (Grape) style by 90th from ImThemes
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.3f;
    style.WindowPadding = ImVec2(6.5f, 2.7f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(20.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.6f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 10.1f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(20.0f, 3.5f);
    style.FrameRounding = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(4.4f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.6f, 3.6f);
    style.CellPadding = ImVec2(3.1f, 6.3f);
    style.IndentSpacing = 4.4f;
    style.ColumnsMinSpacing = 5.4f;
    style.ScrollbarSize = 8.8f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 9.4f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.050980393f, 0.03529412f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.101960786f, 0.101960786f, 0.101960786f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16078432f, 0.14901961f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.54509807f, 0.46666667f, 0.7176471f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.34509805f, 0.29411766f, 0.45882353f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.3137255f, 0.25882354f, 0.42745098f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.31764707f, 0.2784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.41568628f, 0.3647059f, 0.5294118f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.40392157f, 0.3529412f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.3254902f, 0.28627452f, 0.41568628f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.4f, 0.34901962f, 0.5058824f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.2784314f, 0.2509804f, 0.3372549f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void SetupStyle_Photoshop(ImGuiStyle& style)
{
    // Photoshop (Charcoal) style by Derydoca from ImThemes
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 4.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 2.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 13.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabMinSize = 7.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 1.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.27450982f, 0.27450982f, 0.27450982f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.391f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.58431375f, 0.58431375f, 0.58431375f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
}

void ApplyTheme(const char* theme_id, float ui_scale)
{
    const char* id = theme_id;
    if (IsNullOrEmpty(id))
        id = DefaultThemeId();

    if (!IsKnownThemeId(id))
        id = DefaultThemeId();

    // Reset to a clean base so switching themes doesn't leak old values.
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    if (std::strcmp(id, kThemeCherry) == 0)               SetupStyle_Cherry(style);
    else if (std::strcmp(id, kThemeGrape) == 0)           SetupStyle_QuickMinimalLook(style);
    else if (std::strcmp(id, kThemeCharcoal) == 0)        SetupStyle_Photoshop(style);
    else                                                  SetupStyle_Cherry(style);

    // Apply current UI scale (HiDPI).
    if (ui_scale <= 0.0f)
        ui_scale = 1.0f;
    style.ScaleAllSizes(ui_scale);
    style.FontScaleDpi = ui_scale;
}
} // namespace ui


