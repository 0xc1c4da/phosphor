#pragma once

// Shared glyph preview rendering for UI widgets (palette grid, character sets, etc).
//
// This draws using the same bitmap/embedded-font rules as the canvas renderer:
// - If the active canvas has an embedded font and the codepoint is in the embedded PUA range,
//   it draws the indexed glyph from that embedded font.
// - Otherwise, if the canvas uses a bitmap font, it maps Unicode -> CP437 where possible.
// - Otherwise it falls back to drawing the UTF-8 text with ImGui's current font.

#include <cstdint>

struct ImDrawList;
struct ImVec2;

class AnsiCanvas;

// Draw a single glyph into a rectangular cell.
//
// - `p0` is top-left in screen space.
// - `cell_w/h` are in pixels.
// - `cp` is the canvas codepoint (Unicode scalar, including embedded PUA codepoints).
// - `fg_col` is an ImU32 color (e.g. ImGui::GetColorU32(ImGuiCol_Text)).
void DrawGlyphPreview(ImDrawList* dl,
                      const ImVec2& p0,
                      float cell_w,
                      float cell_h,
                      char32_t cp,
                      const AnsiCanvas* canvas,
                      std::uint32_t fg_col);


