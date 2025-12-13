#pragma once

#include "imgui.h"

#include <string>

// A simple IMGUI component: a Play/Pause toggle button + a multiline text editor
// that expands to fill the remaining available space.
class AnslEditor
{
public:
    AnslEditor();

    bool IsPlaying() const { return playing_; }
    void SetPlaying(bool playing) { playing_ = playing; }
    void TogglePlaying() { playing_ = !playing_; }

    std::string&       Text() { return text_; }
    const std::string& Text() const { return text_; }
    void SetText(std::string text) { text_ = std::move(text); }

    // Render the component. `id` must be unique within the current ImGui window.
    // `flags` are passed through to ImGui::InputTextMultiline.
    void Render(const char* id, ImGuiInputTextFlags flags = 0);

private:
    bool        playing_ = false;
    std::string text_;
};
