#pragma once

namespace ui
{
// Stable theme ids (persisted in session.json)
inline constexpr const char* kThemeMoonlight = "moonlight";
inline constexpr const char* kThemeCherry    = "cherry";

// Returns the default theme id (Moonlight).
const char* DefaultThemeId();

// Applies the named theme to ImGui::GetStyle() and then scales style sizes for HiDPI.
// If theme_id is null/empty/unknown, falls back to DefaultThemeId().
void ApplyTheme(const char* theme_id, float ui_scale);

// Helpers for UI.
int         ThemeCount();
const char* ThemeIdByIndex(int idx);
const char* ThemeDisplayName(const char* theme_id);
} // namespace ui


