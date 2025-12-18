#pragma once

namespace ui
{
// Theme ids (persisted in session.json)
inline constexpr const char* kThemeCherry    = "cherry";
inline constexpr const char* kThemeGrape    = "grape";
inline constexpr const char* kThemeCharcoal = "charcoal";

// Returns the default theme id.
const char* DefaultThemeId();

// Applies the named theme to ImGui::GetStyle() and then scales style sizes for HiDPI.
// If theme_id is null/empty/unknown, falls back to DefaultThemeId().
void ApplyTheme(const char* theme_id, float ui_scale);

// Helpers for UI.
int         ThemeCount();
const char* ThemeIdByIndex(int idx);
const char* ThemeDisplayName(const char* theme_id);
} // namespace ui


