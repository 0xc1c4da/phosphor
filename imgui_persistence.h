#pragma once

#include "session_state.h"

// Helpers for persisting ImGui window positions/sizes/collapsed state into SessionState,
// without using ImGui's ini persistence.
//
// Call order (per-window, each frame):
// - before ImGui::Begin(): ApplyImGuiWindowPlacement(...)
// - after Begin() (inside the window scope): CaptureImGuiWindowPlacement(...)

struct ImVec2;

void ApplyImGuiWindowPlacement(const SessionState& session, const char* window_name, bool apply_this_frame);
void CaptureImGuiWindowPlacement(SessionState& session, const char* window_name);


