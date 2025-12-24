#pragma once

// Shared glyph preview rendering for UI widgets (palette grid, character sets, etc).
//
// This draws using the same bitmap/embedded-font rules as the canvas renderer:
// - If the active canvas has an embedded font and the glyph is an EmbeddedIndex token (or a
//   legacy embedded-PUA Unicode scalar), it draws the indexed glyph from that embedded font.
// - Otherwise, if the canvas uses a bitmap font, it maps Unicode -> CP437 where possible for
//   UnicodeScalar glyphs, while BitmapIndex glyphs draw by index.
// - Otherwise it falls back to drawing the UTF-8 text with ImGui's current font.

#include "core/glyph_id.h"

#include <cstdint>

struct ImDrawList;
struct ImVec2;

class AnsiCanvas;

// Draw a single glyph into a rectangular cell.
//
// - `p0` is top-left in screen space.
// - `cell_w/h` are in pixels.
// - `glyph` is the canvas glyph token (Unicode scalar or indexed glyph token).
// - `fg_col` is an ImU32 color (e.g. ImGui::GetColorU32(ImGuiCol_Text)).
void DrawGlyphPreview(ImDrawList* dl,
                      const ImVec2& p0,
                      float cell_w,
                      float cell_h,
                      phos::GlyphId glyph,
                      const AnsiCanvas* canvas,
                      std::uint32_t fg_col);

// Backward-compatible convenience overload: accept a Unicode codepoint.
// Note: legacy embedded PUA values are preserved as UnicodeScalar glyph ids here.
inline void DrawGlyphPreview(ImDrawList* dl,
                             const ImVec2& p0,
                             float cell_w,
                             float cell_h,
                             char32_t cp,
                             const AnsiCanvas* canvas,
                             std::uint32_t fg_col)
{
    DrawGlyphPreview(dl, p0, cell_w, cell_h, phos::glyph::MakeUnicodeScalar(cp), canvas, fg_col);
}


