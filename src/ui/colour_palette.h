#pragma once

#include "imgui.h"

#include <string>
#include <vector>

// Colour palette definition loaded from assets/colours.json:
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


