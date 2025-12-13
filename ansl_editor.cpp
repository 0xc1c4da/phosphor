#include "ansl_editor.h"

#include "imgui.h"

#include <algorithm>
#include <utility>

// ImGui helper to edit std::string via InputText* with automatic resize.
// Adapted from Dear ImGui's imgui_demo.cpp.
static int InputTextCallback_Resize(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* str = static_cast<std::string*>(data->UserData);
        IM_ASSERT(str != nullptr);
        IM_ASSERT(data->Buf == str->c_str());

        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = const_cast<char*>(str->c_str());
    }
    return 0;
}

static bool InputTextMultilineString(const char* label,
                                     std::string* str,
                                     const ImVec2& size,
                                     ImGuiInputTextFlags flags)
{
    IM_ASSERT(str != nullptr);

    // Ensure there is always a NUL-terminated buffer large enough for current contents.
    const size_t min_cap = std::max<size_t>(1024u, str->size() + 1u);
    if (str->capacity() < min_cap)
        str->reserve(min_cap);

    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label,
                                     const_cast<char*>(str->c_str()),
                                     str->capacity() + 1u,
                                     size,
                                     flags,
                                     InputTextCallback_Resize,
                                     str);
}

AnslEditor::AnslEditor()
{
    // Provide a tiny bit of initial capacity so typing doesn't immediately resize every frame.
    text_.reserve(1024);
}

void AnslEditor::Render(const char* id, ImGuiInputTextFlags flags)
{
    if (!id)
        id = "ansl_editor";

    ImGui::PushID(id);

    // Top row: Play/Pause toggle.
    if (ImGui::Button(playing_ ? "Pause" : "Play"))
        playing_ = !playing_;

    ImGui::SameLine();
    ImGui::TextUnformatted(playing_ ? "Playing" : "Paused");

    // Multiline editor filling remaining space.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f) avail.x = 1.0f;
    if (avail.y < 1.0f) avail.y = 1.0f;

    // A hidden label so it doesn't consume layout width; ID uniqueness comes from PushID().
    InputTextMultilineString("##text", &text_, avail, flags);

    ImGui::PopID();
}
