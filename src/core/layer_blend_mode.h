// Core layer blend mode definitions (Phase D groundwork).
// Kept in core (not UI) so the canvas, IO, and UI can share a single enum + naming.
#pragma once

#include <cstdint>
#include <string_view>

namespace phos
{
// v1 compositor: background blends across layers; glyph selection is unchanged (topmost non-space wins);
// foreground colour is blended only for the chosen glyph layer (when fg is set).
enum class LayerBlendMode : std::uint8_t
{
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColourDodge,
    ColourBurn,
};

inline constexpr const char* LayerBlendModeToString(LayerBlendMode m)
{
    switch (m)
    {
        case LayerBlendMode::Normal:     return "normal";
        case LayerBlendMode::Multiply:   return "multiply";
        case LayerBlendMode::Screen:     return "screen";
        case LayerBlendMode::Overlay:    return "overlay";
        case LayerBlendMode::Darken:     return "darken";
        case LayerBlendMode::Lighten:    return "lighten";
        case LayerBlendMode::ColourDodge: return "colour_dodge";
        case LayerBlendMode::ColourBurn:  return "colour_burn";
    }
    return "normal";
}

inline constexpr const char* LayerBlendModeToUiLabel(LayerBlendMode m)
{
    switch (m)
    {
        case LayerBlendMode::Normal:     return "Normal";
        case LayerBlendMode::Multiply:   return "Multiply";
        case LayerBlendMode::Screen:     return "Screen";
        case LayerBlendMode::Overlay:    return "Overlay";
        case LayerBlendMode::Darken:     return "Darken";
        case LayerBlendMode::Lighten:    return "Lighten";
        case LayerBlendMode::ColourDodge: return "Colour Dodge";
        case LayerBlendMode::ColourBurn:  return "Colour Burn";
    }
    return "Normal";
}

inline bool LayerBlendModeFromString(std::string_view s, LayerBlendMode& out)
{
    // Accept a few common spellings for forward/backward safety.
    if (s == "normal") { out = LayerBlendMode::Normal; return true; }
    if (s == "multiply") { out = LayerBlendMode::Multiply; return true; }
    if (s == "screen") { out = LayerBlendMode::Screen; return true; }
    if (s == "overlay") { out = LayerBlendMode::Overlay; return true; }
    if (s == "darken") { out = LayerBlendMode::Darken; return true; }
    if (s == "lighten") { out = LayerBlendMode::Lighten; return true; }
    if (s == "colour_dodge" || s == "dodge" || s == "colourdodge") { out = LayerBlendMode::ColourDodge; return true; }
    if (s == "colour_burn" || s == "burn" || s == "colourburn") { out = LayerBlendMode::ColourBurn; return true; }
    return false;
}

inline LayerBlendMode LayerBlendModeFromInt(std::uint32_t v)
{
    // Unknown -> Normal (safe default).
    switch ((LayerBlendMode)v)
    {
        case LayerBlendMode::Normal:
        case LayerBlendMode::Multiply:
        case LayerBlendMode::Screen:
        case LayerBlendMode::Overlay:
        case LayerBlendMode::Darken:
        case LayerBlendMode::Lighten:
        case LayerBlendMode::ColourDodge:
        case LayerBlendMode::ColourBurn:
            return (LayerBlendMode)v;
    }
    return LayerBlendMode::Normal;
}
} // namespace phos


