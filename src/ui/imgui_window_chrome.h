#pragma once

#include "io/session/session_state.h"

#include "imgui.h"

// Small helper layer for per-window "chrome" features:
// - pin to front/back (z-order)
// - opacity (0..100%)
//
// Intended call order for each window, each frame:
// - before ImGui::Begin(): maybe PushStyleVar(ImGuiStyleVar_Alpha, ...)
// - after Begin() (inside window): Render title-bar context menu + apply z-order
// - after ImGui::End(): PopStyleVar if pushed

ImGuiWindowFlags GetImGuiWindowChromeExtraFlags(const SessionState& session, const char* window_name);

// Push per-window alpha style (from session state). Returns true if it pushed.
bool PushImGuiWindowChromeAlpha(const SessionState* session, const char* window_name);
void PopImGuiWindowChromeAlpha(bool pushed);

// Title-bar right-click menu for pinning + opacity.
void RenderImGuiWindowChromeMenu(SessionState* session, const char* window_name);

// Renders a small button in the title bar, near the standard collapse/close buttons.
// Returns true if clicked. Optionally outputs the button rect in screen space.
//
// Notes:
// - This uses Dear ImGui internals (TitleBarRect), so keep it in this chrome module.
// - This does not open any popup by itself; callers typically OpenPopup() when clicked.
bool RenderImGuiWindowChromeTitleBarButton(const char* id,
                                          const char* label_utf8,
                                          bool has_close_button,
                                          bool has_collapse_button,
                                          ImVec2* out_rect_min = nullptr,
                                          ImVec2* out_rect_max = nullptr);

// Applies the pinned z-order behavior (front/back) for the current window.
void ApplyImGuiWindowChromeZOrder(const SessionState* session, const char* window_name);

// Optional: enforce z-order globally after all windows have been built for the frame.
// This ensures "pinned to front" windows win even if other windows were brought forward during the frame.
void ApplyImGuiWindowChromeGlobalZOrder(const SessionState& session);


