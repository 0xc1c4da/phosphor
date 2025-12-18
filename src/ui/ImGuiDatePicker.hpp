// ImGuiDatePicker
// Upstream: https://github.com/Flix01/imgui/blob/master/addons/imgui_datepicker/
// (Copied from /references/ImGuiDatePicker with small portability tweaks in .cpp)
// License: see /references/ImGuiDatePicker/LICENSE
#pragma once
#include <imgui.h>
#include <string>

#ifndef IMGUI_DATEPICKER_YEAR_MIN
    #define IMGUI_DATEPICKER_YEAR_MIN 1900
#endif // !IMGUI_DATEPICKER_YEAR_MIN

#ifndef IMGUI_DATEPICKER_YEAR_MAX
    #define IMGUI_DATEPICKER_YEAR_MAX 9999
#endif // !IMGUI_DATEPICKER_YEAR_MAX

namespace ImGui
{
    IMGUI_API bool DatePickerEx(const std::string& label, tm& v, ImFont* altFont, bool clampToBorder = false, float itemSpacing = 130.0f);

    IMGUI_API bool DatePicker(const std::string& label, tm& v, bool clampToBorder = false, float itemSpacing = 130.0f);
}


