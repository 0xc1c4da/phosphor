#pragma once

#include "imgui.h"

#include <string>
#include <vector>

// Colour palette definition loaded from assets/color-palettes.json:
// [
//   { "title": "Name", "colors": ["#RRGGBB", "#RRGGBBAA", ...] },
//   ...
// ]
struct ColourPaletteDef
{
    std::string         title;
    std::vector<ImVec4> colors; // normalized RGBA
};

// Loads palette definitions from a JSON file.
// Returns true on success; on failure returns false and sets `error`.
bool LoadColourPalettesFromJson(const char* path,
                                std::vector<ColourPaletteDef>& out,
                                std::string& error);

// Appends a single palette to assets/color-palettes.json (creating a unique title if needed).
// Returns true on success; on failure returns false and sets `error`.
//
// Note: This mutates the JSON file; callers are responsible for reloading any cached palette lists.
bool AppendColourPaletteToJson(const char* path,
                               ColourPaletteDef def,
                               std::string& error);

// UI helper for rendering a colour swatch (palette grid cell) with:
// - Left click / Enter: select "primary" (typically the active FG/BG)
// - Right click / Shift+Enter: select "secondary" (the other of FG/BG)
// - Optional foreground/background selection indicators (outline + corner triangles)
struct ColourPaletteSwatchAction
{
    bool set_primary = false;
    bool set_secondary = false;
};

ColourPaletteSwatchAction RenderColourPaletteSwatchButton(const char* label,
                                                         const ImVec4& color,
                                                         const ImVec2& size,
                                                         bool mark_foreground,
                                                         bool mark_background);


