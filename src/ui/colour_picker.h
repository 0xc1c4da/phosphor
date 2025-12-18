#pragma once

#include "imgui.h"

namespace ImGui
{

// Colour Picker (Xterm-256) discrete pickers with continuous HSV interaction.
// - HueBar: SV square + vertical hue bar (+ optional alpha bar).
// - HueWheel: Hue wheel + SV triangle (+ optional alpha bar).
//
// NOTE: If `palette` is provided, both rendering and output `col[]` are snapped
// to the nearest entry in that palette (by RGB distance). This allows the picker
// to be fully determined & constrained by the currently selected palette (e.g.
// 16/32/64-color palettes).

bool ColorPicker4_Xterm256_HueBar(const char* label, float col[4],
                                  bool show_alpha = false,
                                  bool* out_used_right_click = nullptr,
                                  float* inout_last_hue = nullptr,
                                  const ImVec4* palette = nullptr,
                                  int palette_count = 0);
bool ColorPicker4_Xterm256_HueWheel(const char* label, float col[4],
                                    bool show_alpha = false,
                                    bool* out_used_right_click = nullptr,
                                    float* inout_last_hue = nullptr,
                                    const ImVec4* palette = nullptr,
                                    int palette_count = 0);

// Composite foreground/background widget:
// - Shows two overlaid xterm-quantized color squares (foreground on top of background).
// - Clicking one selects it as active (active_index: 0 = foreground, 1 = background).
// - Top-right swap button exchanges foreground and background colors.
bool XtermForegroundBackgroundWidget(const char* label,
                                     ImVec4& foreground,
                                     ImVec4& background,
                                     int&   active_index);

} // namespace ImGui


