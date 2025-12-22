#include "core/canvas/canvas_internal.h"

#include "core/key_bindings.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
static inline bool IsAsciiItalicCandidate(char32_t cp)
{
    // Conservative: only slant basic ASCII, to avoid distorting box drawing and other glyph art.
    // (We can expand this later once we have font/coverage heuristics.)
    return cp >= 0x20 && cp <= 0x7Eu;
}

static bool RenderItalicGlyphClipped(ImDrawList* draw_list,
                                    ImFont* font,
                                    float font_size,
                                    const ImVec2& top_left,
                                    const ImVec2& clip_min,
                                    const ImVec2& clip_max,
                                    ImU32 col,
                                    char32_t cp)
{
    if (!draw_list || !font)
        return false;
    if (!IsAsciiItalicCandidate(cp))
        return false;

    // Robust synthetic italic:
    // Render the glyph normally (AddText) inside the cell clip rect, then shear the produced
    // vertices. This avoids relying on baked-glyph lookup and atlas quad paths, which can fail
    // depending on font baking lifecycle and zoom-dependent sizes.
    char buf[5] = {0, 0, 0, 0, 0};
    EncodeUtf8(cp, buf);

    const float clip_w = clip_max.x - clip_min.x;
    const float clip_h = clip_max.y - clip_min.y;
    if (!(clip_w > 0.0f) || !(clip_h > 0.0f))
        return false;

    // Bottom-anchored shear (top leans right). Tuned in cell space.
    // shift_x = shear * (cell_bottom_y - y).
    const float shear = 0.20f * (clip_w / clip_h);

    const int vtx_start = draw_list->VtxBuffer.Size;
    draw_list->PushClipRect(clip_min, clip_max, true);
    draw_list->AddText(font, font_size, top_left, col, buf, nullptr);
    draw_list->PopClipRect();
    const int vtx_end = draw_list->VtxBuffer.Size;
    if (vtx_end <= vtx_start)
        return false;

    for (int i = vtx_start; i < vtx_end; ++i)
    {
        ImDrawVert& v = draw_list->VtxBuffer[i];
        v.pos.x += shear * (clip_max.y - v.pos.y);
    }
    return true;
}
} // namespace

// ---- inlined from canvas_render.inc ----
void AnsiCanvas::HandleMouseInteraction(const ImVec2& origin, float cell_w, float cell_h)
{
    EnsureDocument();

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive(); // stays true during click+drag if the item captured the mouse button

    const bool left_down  = io.MouseDown[ImGuiMouseButton_Left];
    const bool right_down = io.MouseDown[ImGuiMouseButton_Right];
    const bool any_down   = left_down || right_down;
    const bool any_clicked =
        (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)));

    // Capture mouse for tool interactions (pencil/brush) so click+drag continues to update
    // even if ImGui ActiveId is owned by another widget (e.g. our hidden InputText).
    if (any_clicked)
        m_mouse_capture = true;
    if (!any_down)
        m_mouse_capture = false;

    const bool tracking = hovered || active || m_mouse_capture;
    if (!tracking)
    {
        m_cursor_valid = false;
        return;
    }

    // Update pointer state (hover cell + pressed state) every frame.
    {
        ImVec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);

        // Convert to cell coords; allow dragging outside the item rect by clamping.
        int col = static_cast<int>(std::floor(local.x / cell_w));
        int row = static_cast<int>(std::floor(local.y / cell_h));

        if (col < 0) col = 0;
        if (col >= m_columns) col = m_columns - 1;
        if (row < 0) row = 0;

        // Don't let hover accidentally grow the document; only allow row growth when interacting.
        // (This keeps keyboard editing stable even if the mouse is moving around.)
        if (!any_down && !any_clicked)
        {
            if (row >= m_rows) row = m_rows - 1;
            if (row < 0) row = 0;
        }
        else
        {
            EnsureRows(row + 1);
        }

        // Derive "half-row" cursor position (Moebius/IcyDraw style).
        // This lets tools decide between upper/lower half blocks without guessing.
        //
        // NOTE: `row` may have been clamped above; clamp the in-cell offset accordingly so
        // half selection remains stable even when dragging outside the grid.
        float in_cell_y = local.y - (float)row * cell_h;
        if (in_cell_y < 0.0f) in_cell_y = 0.0f;
        if (cell_h > 0.0f && in_cell_y >= cell_h) in_cell_y = cell_h - 0.001f;
        const int half_bit = (cell_h > 0.0f && in_cell_y >= (cell_h * 0.5f)) ? 1 : 0;
        const int half_row = row * 2 + half_bit;

        // Previous pointer state (for drag detection).
        m_cursor_pcol = m_cursor_col;
        m_cursor_prow = m_cursor_row;
        m_cursor_phalf_row = m_cursor_half_row;
        m_cursor_prev_left_down  = m_cursor_left_down;
        m_cursor_prev_right_down = m_cursor_right_down;

        // Current pointer state.
        m_cursor_col = col;
        m_cursor_row = row;
        m_cursor_half_row = half_row;
        m_cursor_left_down  = left_down;
        m_cursor_right_down = right_down;
        m_cursor_valid = true;

        // IMPORTANT: tools/scripts decide how mouse input affects the caret.
    }
}

bool AnsiCanvas::GetCursorCell(int& out_x,
                               int& out_y,
                               int& out_half_y,
                               bool& out_left_down,
                               bool& out_right_down,
                               int& out_px,
                               int& out_py,
                               int& out_phalf_y,
                               bool& out_prev_left_down,
                               bool& out_prev_right_down) const
{
    if (!m_cursor_valid)
        return false;
    out_x = m_cursor_col;
    out_y = m_cursor_row;
    out_half_y = m_cursor_half_row;
    out_left_down = m_cursor_left_down;
    out_right_down = m_cursor_right_down;
    out_px = m_cursor_pcol;
    out_py = m_cursor_prow;
    out_phalf_y = m_cursor_phalf_row;
    out_prev_left_down = m_cursor_prev_left_down;
    out_prev_right_down = m_cursor_prev_right_down;
    return true;
}

void AnsiCanvas::DrawVisibleCells(ImDrawList* draw_list,
                                  const ImVec2& origin,
                                  float cell_w,
                                  float cell_h,
                                  float font_size)
{
    if (!draw_list)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    EnsureDocument();

    const int rows = m_rows;
    if (rows <= 0 || m_columns <= 0)
        return;

    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Compute visible cell range based on ImGui's actual clipping rectangle.
    // Using GetWindowContentRegionMin/Max is tempting but becomes subtly wrong under
    // child scrolling + scrollbars; InnerClipRect is what the renderer really clips to.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return;
    const ImRect clip_rect = window->InnerClipRect;
    const ImVec2 clip_min(clip_rect.Min.x, clip_rect.Min.y);
    const ImVec2 clip_max(clip_rect.Max.x, clip_rect.Max.y);

    int start_row = static_cast<int>(std::floor((clip_min.y - origin.y) / cell_h));
    int end_row   = static_cast<int>(std::ceil ((clip_max.y - origin.y) / cell_h));
    int start_col = static_cast<int>(std::floor((clip_min.x - origin.x) / cell_w));
    int end_col   = static_cast<int>(std::ceil ((clip_max.x - origin.x) / cell_w));

    if (start_row < 0) start_row = 0;
    if (start_col < 0) start_col = 0;
    if (end_row > rows) end_row = rows;
    if (end_col > m_columns) end_col = m_columns;

    // Hoist invariants out of the inner loops (hot path).
    const ImU32 paper_bg = m_canvas_bg_white ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
    // The "default" foreground must remain readable regardless of UI skin.
    const ImU32 default_fg = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
    const int caret_row = m_caret_row;
    const int caret_col = m_caret_col;
    const ImU32 caret_fill = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.60f, 0.75f));
    const float now = (float)ImGui::GetTime();
    const bool blink_phase_on = (std::fmod(now, 1.0f) < 0.5f);
    const float deco_thickness = std::max(1.0f, std::floor(cell_h / 16.0f));
    const float underline_y_off = cell_h - deco_thickness;
    const float strike_y_off = std::floor(cell_h * 0.5f - deco_thickness * 0.5f);
    const float bold_dx = std::max(1.0f, std::floor(cell_w / 8.0f));

    auto adjust_intensity = [&](ImU32 c, float mul) -> ImU32
    {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
        v.x = std::clamp(v.x * mul, 0.0f, 1.0f);
        v.y = std::clamp(v.y * mul, 0.0f, 1.0f);
        v.z = std::clamp(v.z * mul, 0.0f, 1.0f);
        return ImGui::ColorConvertFloat4ToU32(v);
    };

    static const ImU32 kVga16[16] = {
        IM_COL32(0x00, 0x00, 0x00, 0xFF), // 0
        IM_COL32(0xAA, 0x00, 0x00, 0xFF), // 1
        IM_COL32(0x00, 0xAA, 0x00, 0xFF), // 2
        IM_COL32(0xAA, 0x55, 0x00, 0xFF), // 3
        IM_COL32(0x00, 0x00, 0xAA, 0xFF), // 4
        IM_COL32(0xAA, 0x00, 0xAA, 0xFF), // 5
        IM_COL32(0x00, 0xAA, 0xAA, 0xFF), // 6
        IM_COL32(0xAA, 0xAA, 0xAA, 0xFF), // 7
        IM_COL32(0x55, 0x55, 0x55, 0xFF), // 8
        IM_COL32(0xFF, 0x55, 0x55, 0xFF), // 9
        IM_COL32(0x55, 0xFF, 0x55, 0xFF), // 10
        IM_COL32(0xFF, 0xFF, 0x55, 0xFF), // 11
        IM_COL32(0x55, 0x55, 0xFF, 0xFF), // 12
        IM_COL32(0xFF, 0x55, 0xFF, 0xFF), // 13
        IM_COL32(0x55, 0xFF, 0xFF, 0xFF), // 14
        IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), // 15
    };
    auto vga16_index = [&](ImU32 c, int& out_idx) -> bool
    {
        for (int i = 0; i < 16; ++i)
        {
            if (kVga16[i] == c)
            {
                out_idx = i;
                return true;
            }
        }
        out_idx = 0;
        return false;
    };

    float y = origin.y + (float)start_row * cell_h;
    for (int row = start_row; row < end_row; ++row, y += cell_h)
    {
        float x = origin.x + (float)start_col * cell_w;
        for (int col = start_col; col < end_col; ++col, x += cell_w)
        {
            ImVec2 cell_min(x, y);
            ImVec2 cell_max(x + cell_w, y + cell_h);

            CompositeCell cell = GetCompositeCell(row, col);

            // Resolve base fg/bg (note: bg==0 means "unset/transparent" in the editor).
            ImU32 fg_col = (cell.fg != 0) ? (ImU32)cell.fg : default_fg;
            ImU32 bg_col = (cell.bg != 0) ? (ImU32)cell.bg : paper_bg;

            const Attrs a = cell.attrs;
            const bool reverse = (a & Attr_Reverse) != 0;
            if (reverse)
            {
                // If both colors are exact VGA16 palette entries, emulate libansilove's
                // special reverse rule that preserves the bright-foreground bit.
                int fi = 0, bi = 0;
                // IMPORTANT: only apply the VGA16 special case when both fg/bg are explicitly set.
                // Unset channels are represented as 0, which would otherwise spuriously match VGA16 black.
                if (cell.fg != 0 && cell.bg != 0 && vga16_index((ImU32)cell.fg, fi) && vga16_index((ImU32)cell.bg, bi))
                {
                    const int inv_bg = fi % 8;
                    const int inv_fg = bi + (fi & 8);
                    bg_col = kVga16[std::clamp(inv_bg, 0, 15)];
                    fg_col = kVga16[std::clamp(inv_fg, 0, 15)];
                }
                else
                {
                    std::swap(fg_col, bg_col);
                }
            }
            if ((a & Attr_Dim) != 0)
                fg_col = adjust_intensity(fg_col, 0.60f);
            if ((a & Attr_Bold) != 0)
                fg_col = adjust_intensity(fg_col, 1.25f);

            // Background fill:
            // - normally, only fill when bg is explicitly set
            // - in reverse mode, fill using the effective swapped bg
            if (cell.bg != 0 || reverse)
                draw_list->AddRectFilled(cell_min, cell_max, ApplyCurrentStyleAlpha(bg_col));

            // Caret highlight.
            if (row == caret_row && col == caret_col)
            {
                draw_list->AddRectFilled(cell_min, cell_max, caret_fill);
            }

            // Blink (SGR 5): blink foreground/attributes only (background remains).
            const bool blink = (a & Attr_Blink) != 0;
            const bool blink_on = !blink || blink_phase_on;

            const bool want_underline = (a & Attr_Underline) != 0;
            const bool want_strike = (a & Attr_Strikethrough) != 0;

            // Underline / strikethrough (draw even for spaces).
            if (blink_on && (want_underline || want_strike))
            {
                const ImU32 lc = ApplyCurrentStyleAlpha(fg_col);
                if (want_underline)
                {
                    const float y0 = cell_min.y + underline_y_off;
                    draw_list->AddRectFilled(ImVec2(cell_min.x, y0), ImVec2(cell_max.x, y0 + deco_thickness), lc);
                }
                if (want_strike)
                {
                    const float y0 = cell_min.y + strike_y_off;
                    draw_list->AddRectFilled(ImVec2(cell_min.x, y0), ImVec2(cell_max.x, y0 + deco_thickness), lc);
                }
            }

            const char32_t cp = cell.cp;
            if (cp == U' ' || !blink_on)
            {
                // Space glyphs draw nothing unless bg (handled above) or underline/strike (handled above).
                // Blinking "off" suppresses glyph rendering (but background remains).
                continue;
            }

            if (!bitmap_font)
            {
                char buf[5] = {0, 0, 0, 0, 0};
                EncodeUtf8(cp, buf);
                const ImU32 text_col = ApplyCurrentStyleAlpha(fg_col);
                const bool italic = (a & Attr_Italic) != 0;
                const bool bold = (a & Attr_Bold) != 0;

                // Clip text to cell: required for synthetic bold/italic to avoid bleeding.
                draw_list->PushClipRect(cell_min, cell_max, true);

                auto draw_once = [&](float dx)
                {
                    const ImVec2 p(cell_min.x + dx, cell_min.y);
                    if (!(italic && RenderItalicGlyphClipped(draw_list, font, font_size,
                                                            p, cell_min, cell_max,
                                                            text_col, cp)))
                    {
                        draw_list->AddText(font, font_size, p, text_col, buf, nullptr);
                    }
                };

                draw_once(0.0f);
                if (bold)
                    draw_once(bold_dx);

                draw_list->PopClipRect();
            }
            else
            {
                // Bitmap path:
                // - If an embedded font is present, interpret U+E000.. as glyph indices.
                // - Otherwise map Unicode -> CP437 glyph index (0..255) in the selected bitmap font.
                int glyph_cell_w = finfo.cell_w;
                int glyph_cell_h = finfo.cell_h;
                bool vga_dup = finfo.vga_9col_dup;

                std::uint16_t glyph_index = 0;
                if (embedded_font)
                {
                    glyph_cell_w = ef->cell_w;
                    glyph_cell_h = ef->cell_h;
                    vga_dup = ef->vga_9col_dup;

                    if (cp >= kEmbeddedGlyphBase && cp < (kEmbeddedGlyphBase + (char32_t)ef->glyph_count))
                    {
                        glyph_index = (std::uint16_t)(cp - kEmbeddedGlyphBase);
                    }
                    else
                    {
                        // Best-effort: if the embedded font is CP437-ordered, map Unicode to CP437.
                        std::uint8_t b = 0;
                        if (fonts::UnicodeToCp437Byte(cp, b))
                            glyph_index = (std::uint16_t)b;
                        else
                            glyph_index = (std::uint16_t)'?';
                    }
                }
                else
                {
                    std::uint8_t glyph = 0;
                    if (!fonts::UnicodeToCp437Byte(cp, glyph))
                    {
                        // Fallbacks: prefer '?' if representable, otherwise space.
                        std::uint8_t q = 0;
                        glyph = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
                    }
                    glyph_index = (std::uint16_t)glyph;
                }

                auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
                {
                    if (embedded_font)
                    {
                        if (gi >= (std::uint16_t)ef->glyph_count) return 0;
                        if (yy < 0 || yy >= ef->cell_h) return 0;
                        return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
                    }
                    return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
                };

                const float px_w = cell_w / (float)std::max(1, glyph_cell_w);
                const float px_h = cell_h / (float)std::max(1, glyph_cell_h);
                const ImU32 col = ApplyCurrentStyleAlpha(fg_col);
                const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);
                const bool bold = (a & Attr_Bold) != 0;
                const bool italic = ((a & Attr_Italic) != 0) && IsAsciiItalicCandidate(cp);
                const float shear = italic ? (0.20f * (cell_w / std::max(1.0f, cell_h))) : 0.0f;

                for (int yy = 0; yy < glyph_cell_h; ++yy)
                {
                    std::uint8_t bits = glyph_row_bits(glyph_index, yy);
                    if (bold)
                        bits = (std::uint8_t)(bits | (bits >> 1)); // 1px dilation to the right
                    int run_start = -1;
                    auto bit_set = [&](int x) -> bool
                    {
                        if (x < 0)
                            return false;
                        if (x < 8)
                            return (bits & (std::uint8_t)(0x80u >> x)) != 0;
                        if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph8 >= 192 && glyph8 <= 223)
                            return (bits & 0x01u) != 0; // x==7 is LSB when shifting 0x80>>7
                        return false;
                    };

                    for (int xx = 0; xx < glyph_cell_w; ++xx)
                    {
                        const bool on = bit_set(xx);
                        if (on && run_start < 0)
                            run_start = xx;
                        if ((!on || xx == glyph_cell_w - 1) && run_start >= 0)
                        {
                            const int run_end = on ? (xx + 1) : xx; // exclusive
                            float x0 = cell_min.x + (float)run_start * px_w;
                            float x1 = cell_min.x + (float)run_end * px_w;
                            const float y0 = cell_min.y + (float)yy * px_h;
                            const float y1 = cell_min.y + (float)(yy + 1) * px_h;
                            if (italic)
                            {
                                const float y_mid = 0.5f * (y0 + y1);
                                const float shift = shear * (cell_max.y - y_mid);
                                x0 += shift;
                                x1 += shift;
                            }

                            // Clamp horizontally so italic/bold never bleeds into neighbors.
                            x0 = std::max(x0, cell_min.x);
                            x1 = std::min(x1, cell_max.x);
                            (void)y0;
                            (void)y1;
                            if (x1 > x0)
                            {
                                draw_list->AddRectFilled(ImVec2(x0, cell_min.y + (float)yy * px_h),
                                                         ImVec2(x1, cell_min.y + (float)(yy + 1) * px_h),
                                                         col);
                            }
                            run_start = -1;
                        }
                    }
                }
            }
        }
    }
}

void AnsiCanvas::DrawSelectionOverlay(ImDrawList* draw_list,
                                      const ImVec2& origin,
                                      float cell_w,
                                      float cell_h,
                                      float font_size)
{
    if (!draw_list)
        return;
    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Floating selection preview (drawn above the document).
    if (m_move.active && m_move.w > 0 && m_move.h > 0 && (int)m_move.cells.size() == m_move.w * m_move.h)
    {
        const int w = m_move.w;
        const int h = m_move.h;
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                const int x = m_move.dst_x + i;
                const int y = m_move.dst_y + j;
                if (x < 0 || x >= m_columns || y < 0 || y >= m_rows)
                    continue;

                const ClipCell& c = m_move.cells[(size_t)j * (size_t)w + (size_t)i];
                ImVec2 cell_min(origin.x + x * cell_w,
                                origin.y + y * cell_h);
                ImVec2 cell_max(cell_min.x + cell_w,
                                cell_min.y + cell_h);
                const ImU32 paper_bg = m_canvas_bg_white ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
                const ImU32 default_fg = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

                ImU32 fg_col = (c.fg != 0) ? (ImU32)c.fg : default_fg;
                ImU32 bg_col = (c.bg != 0) ? (ImU32)c.bg : paper_bg;

                const Attrs a = c.attrs;
                const bool reverse = (a & Attr_Reverse) != 0;
                if (reverse)
                {
                    // Apply libansilove-compatible VGA16 reverse rule when both channels are explicitly set
                    // and exactly match VGA16 palette entries. Otherwise do a normal swap on the effective colors.
                    auto vga16_index = [&](ImU32 col, int& out_idx) -> bool
                    {
                        static const ImU32 vga16[16] = {
                            IM_COL32(0x00, 0x00, 0x00, 0xFF), IM_COL32(0xAA, 0x00, 0x00, 0xFF),
                            IM_COL32(0x00, 0xAA, 0x00, 0xFF), IM_COL32(0xAA, 0x55, 0x00, 0xFF),
                            IM_COL32(0x00, 0x00, 0xAA, 0xFF), IM_COL32(0xAA, 0x00, 0xAA, 0xFF),
                            IM_COL32(0x00, 0xAA, 0xAA, 0xFF), IM_COL32(0xAA, 0xAA, 0xAA, 0xFF),
                            IM_COL32(0x55, 0x55, 0x55, 0xFF), IM_COL32(0xFF, 0x55, 0x55, 0xFF),
                            IM_COL32(0x55, 0xFF, 0x55, 0xFF), IM_COL32(0xFF, 0xFF, 0x55, 0xFF),
                            IM_COL32(0x55, 0x55, 0xFF, 0xFF), IM_COL32(0xFF, 0x55, 0xFF, 0xFF),
                            IM_COL32(0x55, 0xFF, 0xFF, 0xFF), IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),
                        };
                        for (int i = 0; i < 16; ++i)
                        {
                            if (vga16[i] == col)
                            {
                                out_idx = i;
                                return true;
                            }
                        }
                        out_idx = 0;
                        return false;
                    };
                    int fi = 0, bi = 0;
                    if (c.fg != 0 && c.bg != 0 && vga16_index((ImU32)c.fg, fi) && vga16_index((ImU32)c.bg, bi))
                    {
                        const int inv_bg = fi % 8;
                        const int inv_fg = bi + (fi & 8);
                        static const ImU32 vga16[16] = {
                            IM_COL32(0x00, 0x00, 0x00, 0xFF), IM_COL32(0xAA, 0x00, 0x00, 0xFF),
                            IM_COL32(0x00, 0xAA, 0x00, 0xFF), IM_COL32(0xAA, 0x55, 0x00, 0xFF),
                            IM_COL32(0x00, 0x00, 0xAA, 0xFF), IM_COL32(0xAA, 0x00, 0xAA, 0xFF),
                            IM_COL32(0x00, 0xAA, 0xAA, 0xFF), IM_COL32(0xAA, 0xAA, 0xAA, 0xFF),
                            IM_COL32(0x55, 0x55, 0x55, 0xFF), IM_COL32(0xFF, 0x55, 0x55, 0xFF),
                            IM_COL32(0x55, 0xFF, 0x55, 0xFF), IM_COL32(0xFF, 0xFF, 0x55, 0xFF),
                            IM_COL32(0x55, 0x55, 0xFF, 0xFF), IM_COL32(0xFF, 0x55, 0xFF, 0xFF),
                            IM_COL32(0x55, 0xFF, 0xFF, 0xFF), IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),
                        };
                        bg_col = vga16[std::clamp(inv_bg, 0, 15)];
                        fg_col = vga16[std::clamp(inv_fg, 0, 15)];
                    }
                    else
                    {
                        std::swap(fg_col, bg_col);
                    }
                }

                auto adjust_intensity = [&](ImU32 col, float mul) -> ImU32
                {
                    ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
                    v.x = std::clamp(v.x * mul, 0.0f, 1.0f);
                    v.y = std::clamp(v.y * mul, 0.0f, 1.0f);
                    v.z = std::clamp(v.z * mul, 0.0f, 1.0f);
                    return ImGui::ColorConvertFloat4ToU32(v);
                };
                if ((a & Attr_Dim) != 0)
                    fg_col = adjust_intensity(fg_col, 0.60f);
                if ((a & Attr_Bold) != 0)
                    fg_col = adjust_intensity(fg_col, 1.25f);

                if (c.bg != 0 || reverse)
                    draw_list->AddRectFilled(cell_min, cell_max, ApplyCurrentStyleAlpha(bg_col));

                const bool blink = (a & Attr_Blink) != 0;
                const bool blink_on = !blink || (std::fmod((float)ImGui::GetTime(), 1.0f) < 0.5f);
                const bool want_underline = (a & Attr_Underline) != 0;
                const bool want_strike = (a & Attr_Strikethrough) != 0;
                if (blink_on && (want_underline || want_strike))
                {
                    const float thickness = std::max(1.0f, std::floor(cell_h / 16.0f));
                    const ImU32 lc = ApplyCurrentStyleAlpha(fg_col);
                    if (want_underline)
                    {
                        const float y0 = cell_max.y - thickness;
                        draw_list->AddRectFilled(ImVec2(cell_min.x, y0), ImVec2(cell_max.x, y0 + thickness), lc);
                    }
                    if (want_strike)
                    {
                        const float y0 = cell_min.y + std::floor(cell_h * 0.5f - thickness * 0.5f);
                        draw_list->AddRectFilled(ImVec2(cell_min.x, y0), ImVec2(cell_max.x, y0 + thickness), lc);
                    }
                }

                if (c.cp == U' ' || !blink_on)
                    continue;

                if (!bitmap_font)
                {
                    char buf[5] = {0, 0, 0, 0, 0};
                    EncodeUtf8(c.cp, buf);
                    const ImU32 text_col = ApplyCurrentStyleAlpha(fg_col);
                    const bool italic = (a & Attr_Italic) != 0;
                    const bool bold = (a & Attr_Bold) != 0;
                    const float bold_dx = std::max(1.0f, std::floor(cell_w / 8.0f));

                    draw_list->PushClipRect(cell_min, cell_max, true);

                    auto draw_once = [&](float dx)
                    {
                        const ImVec2 p(cell_min.x + dx, cell_min.y);
                        if (!(italic && RenderItalicGlyphClipped(draw_list, font, font_size,
                                                                p, cell_min, cell_max,
                                                                text_col, c.cp)))
                        {
                            draw_list->AddText(font, font_size, p, text_col, buf, nullptr);
                        }
                    };

                    draw_once(0.0f);
                    if (bold)
                        draw_once(bold_dx);

                    draw_list->PopClipRect();
                }
                else
                {
                        int glyph_cell_w = finfo.cell_w;
                        int glyph_cell_h = finfo.cell_h;
                        bool vga_dup = finfo.vga_9col_dup;

                        std::uint16_t glyph_index = 0;
                        if (embedded_font)
                        {
                            glyph_cell_w = ef->cell_w;
                            glyph_cell_h = ef->cell_h;
                            vga_dup = ef->vga_9col_dup;
                            if (c.cp >= kEmbeddedGlyphBase && c.cp < (kEmbeddedGlyphBase + (char32_t)ef->glyph_count))
                                glyph_index = (std::uint16_t)(c.cp - kEmbeddedGlyphBase);
                            else
                            {
                                std::uint8_t b = 0;
                                if (fonts::UnicodeToCp437Byte(c.cp, b))
                                    glyph_index = (std::uint16_t)b;
                                else
                                    glyph_index = (std::uint16_t)'?';
                            }
                        }
                        else
                        {
                            std::uint8_t glyph = 0;
                            if (!fonts::UnicodeToCp437Byte(c.cp, glyph))
                            {
                                std::uint8_t q = 0;
                                glyph = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
                            }
                            glyph_index = (std::uint16_t)glyph;
                        }

                        auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
                        {
                            if (embedded_font)
                            {
                                if (gi >= (std::uint16_t)ef->glyph_count) return 0;
                                if (yy < 0 || yy >= ef->cell_h) return 0;
                                return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
                            }
                            return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
                        };

                        const float px_w = cell_w / (float)std::max(1, glyph_cell_w);
                        const float px_h = cell_h / (float)std::max(1, glyph_cell_h);
                        const ImU32 col = ApplyCurrentStyleAlpha(fg_col);
                        const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);
                        const bool bold = (a & Attr_Bold) != 0;
                        const bool italic = ((a & Attr_Italic) != 0) && IsAsciiItalicCandidate(c.cp);
                        const float shear = italic ? (0.20f * (cell_w / std::max(1.0f, cell_h))) : 0.0f;

                        for (int yy = 0; yy < glyph_cell_h; ++yy)
                        {
                            std::uint8_t bits = glyph_row_bits(glyph_index, yy);
                            if (bold)
                                bits = (std::uint8_t)(bits | (bits >> 1));
                            int run_start = -1;
                            auto bit_set = [&](int x) -> bool
                            {
                                if (x < 0)
                                    return false;
                                if (x < 8)
                                    return (bits & (std::uint8_t)(0x80u >> x)) != 0;
                                if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph8 >= 192 && glyph8 <= 223)
                                    return (bits & 0x01u) != 0;
                                return false;
                            };

                            for (int xx = 0; xx < glyph_cell_w; ++xx)
                            {
                                const bool on = bit_set(xx);
                                if (on && run_start < 0)
                                    run_start = xx;
                                if ((!on || xx == glyph_cell_w - 1) && run_start >= 0)
                                {
                                    const int run_end = on ? (xx + 1) : xx; // exclusive
                                    float x0 = cell_min.x + (float)run_start * px_w;
                                    float x1 = cell_min.x + (float)run_end * px_w;
                                    const float y0 = cell_min.y + (float)yy * px_h;
                                    const float y1 = cell_min.y + (float)(yy + 1) * px_h;

                                    if (italic)
                                    {
                                        const float y_mid = 0.5f * (y0 + y1);
                                        const float shift = shear * (cell_max.y - y_mid);
                                        x0 += shift;
                                        x1 += shift;
                                    }

                                    x0 = std::max(x0, cell_min.x);
                                    x1 = std::min(x1, cell_max.x);
                                    if (x1 > x0)
                                    {
                                        draw_list->AddRectFilled(ImVec2(x0, y0),
                                                                 ImVec2(x1, y1),
                                                                 col);
                                    }
                                    run_start = -1;
                                }
                            }
                        }
                }
            }
        }
    }

    // Selection border (uses selection rect, which tracks floating selection during move).
    if (HasSelection())
    {
        const int x0 = m_selection.x;
        const int y0 = m_selection.y;
        const int x1 = x0 + m_selection.w;
        const int y1 = y0 + m_selection.h;

        ImVec2 p0(origin.x + x0 * cell_w, origin.y + y0 * cell_h);
        ImVec2 p1(origin.x + x1 * cell_w, origin.y + y1 * cell_h);
        p0.x = std::floor(p0.x) + 0.5f;
        p0.y = std::floor(p0.y) + 0.5f;
        p1.x = std::floor(p1.x) - 0.5f;
        p1.y = std::floor(p1.y) - 0.5f;

        const ImU32 col = ImGui::GetColorU32(ImVec4(0.15f, 0.75f, 1.0f, 0.90f));
        draw_list->AddRect(p0, p1, col, 0.0f, 0, 2.0f);
    }
}

void AnsiCanvas::Render(const char* id)
{
    Render(id, {});
}

void AnsiCanvas::Render(const char* id, const std::function<void(AnsiCanvas& canvas, int phase)>& tool_runner)
{
    if (!id)
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    EnsureDocument();

    // Zoom stabilization:
    // Track whether zoom changed recently, and keep layout decisions stable for a few frames.
    // This prevents scrollbar toggling on rounding thresholds (InnerClipRect changes => flicker/jitter).
    const bool zoom_changed_since_last_frame = (m_last_view.valid && m_last_view.zoom != m_zoom);
    if (zoom_changed_since_last_frame)
        m_zoom_stabilize_frames = 6; // ~100ms at 60fps; enough to cover discrete trackpad steps
    else if (m_zoom_stabilize_frames > 0)
        --m_zoom_stabilize_frames;
    const bool zoom_stabilizing = (m_zoom_stabilize_frames > 0);

    // Base cell size:
    // - For Unscii (ImGui atlas): use the current ImGui font metrics.
    // - For bitmap fonts: use the selected font's textmode cell metrics, scaled by the
    //   current ImGui font size so HiDPI stays consistent with the rest of the UI.
    //
    // We intentionally *do not auto-fit to window width*; the user controls zoom explicitly.
    const float base_font_size = ImGui::GetFontSize();
    const fonts::FontInfo& finfo = fonts::Get(GetFontId());
    const EmbeddedBitmapFont* ef = GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    float cell_w = 0.0f;
    float cell_h = 0.0f;
    if (embedded_font)
    {
        const float base_scale = base_font_size / 16.0f;
        cell_w = (float)ef->cell_w * base_scale;
        cell_h = (float)ef->cell_h * base_scale;
    }
    else if (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0)
    {
        const float base_scale = base_font_size / 16.0f;
        cell_w = (float)finfo.cell_w * base_scale;
        cell_h = (float)finfo.cell_h * base_scale;
    }
    else
    {
        cell_w = font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
        cell_h = base_font_size;
    }

    // Quick status line (foundation for future toolbars).
    if (m_status_line_visible)
    {
        ImGui::PushID(id);
        bool status_editing = false;

        // With the canvas window rendered full-bleed (zero WindowPadding), add a tiny
        // amount of breathing room for the status line only.
        const ImGuiStyle& style_status = ImGui::GetStyle();
        const float status_pad_x = std::max(0.0f, style_status.FramePadding.x);
        const float status_pad_y = std::max(0.0f, style_status.FramePadding.y * 0.5f);
        if (status_pad_y > 0.0f)
            ImGui::Dummy(ImVec2(0.0f, status_pad_y));
        if (status_pad_x > 0.0f)
            ImGui::Indent(status_pad_x);

        const ImGuiInputTextFlags num_flags =
            ImGuiInputTextFlags_CharsDecimal |
            ImGuiInputTextFlags_AutoSelectAll;

        auto sync_buf = [&](const char* label, char* buf, size_t buf_sz, int value)
        {
            const ImGuiID wid = ImGui::GetID(label);
            if (ImGui::GetActiveID() == wid)
                return;
            std::snprintf(buf, buf_sz, "%d", value);
        };

        auto parse_int = [&](const char* buf, int& out) -> bool
        {
            if (!buf || !*buf)
                return false;
            char* end = nullptr;
            long v = std::strtol(buf, &end, 10);
            if (end == buf)
                return false;
            if (v < (long)std::numeric_limits<int>::min()) v = (long)std::numeric_limits<int>::min();
            if (v > (long)std::numeric_limits<int>::max()) v = (long)std::numeric_limits<int>::max();
            out = (int)v;
            return true;
        };

        const ImGuiStyle& style = ImGui::GetStyle();
        const float w_int =
            std::max(90.0f,
                     ImGui::CalcTextSize("000000").x + style.FramePadding.x * 2.0f);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Cols:");
        ImGui::SameLine();
        ImGui::PushItemWidth(w_int);
        sync_buf("##cols", m_status_cols_buf, sizeof(m_status_cols_buf), m_columns);
        ImGui::InputText("##cols", m_status_cols_buf, sizeof(m_status_cols_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int v = m_columns;
            if (parse_int(m_status_cols_buf, v))
            {
                if (v < 1) v = 1;
                if (v != m_columns)
                    SetColumns(v);
                std::snprintf(m_status_cols_buf, sizeof(m_status_cols_buf), "%d", m_columns);
            }
            else
            {
                std::snprintf(m_status_cols_buf, sizeof(m_status_cols_buf), "%d", m_columns);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Rows:");
        ImGui::SameLine();
        ImGui::PushItemWidth(w_int);
        sync_buf("##rows", m_status_rows_buf, sizeof(m_status_rows_buf), m_rows);
        ImGui::InputText("##rows", m_status_rows_buf, sizeof(m_status_rows_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int v = m_rows;
            if (parse_int(m_status_rows_buf, v))
            {
                if (v < 1) v = 1;
                if (v != m_rows)
                    SetRows(v);
                std::snprintf(m_status_rows_buf, sizeof(m_status_rows_buf), "%d", m_rows);
            }
            else
            {
                std::snprintf(m_status_rows_buf, sizeof(m_status_rows_buf), "%d", m_rows);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("Caret:");
        ImGui::SameLine();
        ImGui::TextUnformatted("(");
        ImGui::SameLine();

        ImGui::PushItemWidth(w_int);
        sync_buf("##caret_x", m_status_caret_x_buf, sizeof(m_status_caret_x_buf), m_caret_col);
        ImGui::InputText("##caret_x", m_status_caret_x_buf, sizeof(m_status_caret_x_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int x = m_caret_col;
            if (parse_int(m_status_caret_x_buf, x))
            {
                if (x < 0) x = 0;
                if (m_columns > 0 && x >= m_columns) x = m_columns - 1;
                SetCaretCell(x, m_caret_row);
                std::snprintf(m_status_caret_x_buf, sizeof(m_status_caret_x_buf), "%d", m_caret_col);
            }
            else
            {
                std::snprintf(m_status_caret_x_buf, sizeof(m_status_caret_x_buf), "%d", m_caret_col);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(",");
        ImGui::SameLine();

        ImGui::PushItemWidth(w_int);
        sync_buf("##caret_y", m_status_caret_y_buf, sizeof(m_status_caret_y_buf), m_caret_row);
        ImGui::InputText("##caret_y", m_status_caret_y_buf, sizeof(m_status_caret_y_buf), num_flags);
        ImGui::PopItemWidth();
        status_editing = status_editing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int y = m_caret_row;
            if (parse_int(m_status_caret_y_buf, y))
            {
                if (y < 0) y = 0;
                // Keep caret within current canvas rows; resize first if you want to move beyond.
                if (m_rows > 0 && y >= m_rows) y = m_rows - 1;
                SetCaretCell(m_caret_col, y);
                std::snprintf(m_status_caret_y_buf, sizeof(m_status_caret_y_buf), "%d", m_caret_row);
            }
            else
            {
                std::snprintf(m_status_caret_y_buf, sizeof(m_status_caret_y_buf), "%d", m_caret_row);
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(")");

        // Right-aligned "Edit SAUCE…" button (requested).
        {
            const char* btn_label = "Edit SAUCE...";
            const float btn_w = ImGui::CalcTextSize(btn_label).x + style.FramePadding.x * 2.0f;
            const float right_x = ImGui::GetWindowContentRegionMax().x; // window-local

            ImGui::SameLine();
            // Canvas font combo lives left of the background toggle + SAUCE button.
            const float combo_w = 240.0f;
            const float sq = ImGui::GetFrameHeight();
            const float total_w = combo_w + style.ItemSpacing.x + sq + style.ItemSpacing.x + btn_w;

            float x = right_x - total_w;
            // Avoid going backwards too aggressively; this is a best-effort alignment.
            if (x > ImGui::GetCursorPosX())
                ImGui::SetCursorPosX(x);

            {
                ImGui::SetNextItemWidth(combo_w);
                const fonts::FontId cur = GetFontId();
                const fonts::FontInfo& cur_info = fonts::Get(cur);
                // If the canvas has a valid embedded bitmap font (e.g. XBin), the renderer will
                // always prefer it over the selected SAUCE FontName. Reflect that in the UI so
                // the dropdown doesn't misleadingly show "Unscii" (or any other FontName).
                std::string embedded_preview;
                const char* preview = (cur_info.label && *cur_info.label) ? cur_info.label : "(unknown)";
                if (embedded_font)
                {
                    char buf[96];
                    const int cw = ef ? ef->cell_w : 0;
                    const int ch = ef ? ef->cell_h : 0;
                    const int gc = ef ? ef->glyph_count : 0;
                    std::snprintf(buf, sizeof(buf), "Embedded %dx%d (%d glyphs)", cw, ch, gc);
                    embedded_preview = buf;
                    preview = embedded_preview.c_str();
                }

                if (ImGui::BeginCombo("##canvas_font_combo", preview))
                {
                    if (embedded_font)
                    {
                        ImGui::BeginDisabled();
                        ImGui::Selectable(preview, true);
                        ImGui::EndDisabled();
                        ImGui::Separator();
                        ImGui::BeginDisabled();
                    }

                    for (const auto& f : fonts::AllFonts())
                    {
                        const bool selected = (f.id == cur);
                        if (ImGui::Selectable(f.label ? f.label : "(unnamed)", selected))
                        {
                            (void)SetFontId(f.id);
                            status_editing = true; // prevent hidden input focus this frame
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }

                    if (embedded_font)
                    {
                        ImGui::EndDisabled();
                        ImGui::Separator();
                        ImGui::TextDisabled("Embedded font is active (from the imported file).");
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine();
            // Canvas background toggle square (black/white) lives just left of the SAUCE button.
            const ImVec4 bg_col = m_canvas_bg_white ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
            ImGuiColorEditFlags cflags =
                ImGuiColorEditFlags_NoTooltip |
                ImGuiColorEditFlags_NoAlpha |
                ImGuiColorEditFlags_NoPicker |
                ImGuiColorEditFlags_NoDragDrop;
            if (ImGui::ColorButton("##canvas_bg", bg_col, cflags, ImVec2(sq, sq)))
            {
                ToggleCanvasBackgroundWhite();
                status_editing = true; // prevent the hidden input widget from stealing focus this frame
            }
            // Outline for visibility regardless of theme.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetItemRectMin();
                const ImVec2 p1 = ImGui::GetItemRectMax();
                const ImU32 outline = m_canvas_bg_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
                dl->AddRect(p0, p1, ApplyCurrentStyleAlpha(outline));
            }

            ImGui::SameLine();
            if (ImGui::Button(btn_label))
            {
                m_request_open_sauce_editor = true;
                status_editing = true; // prevent the hidden input widget from stealing focus this frame
            }
        }

        // Tell the hidden canvas text-input widget to stand down while the user edits these fields.
        // Also drop canvas focus so tools don't react to keystrokes during numeric entry.
        m_status_bar_editing = status_editing;
        if (status_editing)
            m_has_focus = false;

        if (status_pad_x > 0.0f)
            ImGui::Unindent(status_pad_x);
        if (status_pad_y > 0.0f)
            ImGui::Dummy(ImVec2(0.0f, status_pad_y));

        ImGui::PopID();
    }

    // Hidden input widget to reliably receive UTF-8 text events from SDL3.
    //
    // IMPORTANT: this must NOT live inside the scrollable canvas child. If it does,
    // forcing keyboard focus to it (SetKeyboardFocusHere) will cause ImGui to scroll
    // the child to reveal the focused item, which feels like the canvas "jumps" to
    // the top when you click/paint while scrolled.
    //
    // Also IMPORTANT: do not let this widget alter layout or become visible (caret '|').
    // We render it off-screen and restore cursor pos so the canvas placement is unchanged.
    if (!m_status_bar_editing)
    {
        const ImVec2 saved = ImGui::GetCursorPos();
        const float line_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::SetCursorPos(ImVec2(-10000.0f, saved.y - line_h));
        HandleCharInputWidget(id);
        ImGui::SetCursorPos(saved);
    }

    // Layer GUI lives in the LayerManager component (see layer_manager.*).

    // Scrollable region: fixed-width canvas, "infinite" rows (grown on demand).
    std::string child_id = std::string(id) + "##_scroll";
    ImGuiWindowFlags child_flags =
        ImGuiWindowFlags_HorizontalScrollbar |
        ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus;
    // During zoom changes, force scrollbars to remain present so the viewport (InnerClipRect)
    // dimensions stay stable. This avoids a common flicker source where the vertical scrollbar
    // toggles on/off across rounding thresholds.
    if (zoom_stabilizing)
    {
        child_flags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
        child_flags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
    }
    // Canvas "paper" background is independent of the UI theme, so also override the
    // child window background (covers areas outside the grid, e.g. when the canvas is small).
    const ImVec4 canvas_bg = m_canvas_bg_white ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, canvas_bg);
    // No child border: it reads as a margin/frame around the canvas, especially on white.
    if (!ImGui::BeginChild(child_id.c_str(), ImVec2(0, 0), false, child_flags))
    {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    const float base_cell_w    = cell_w;
    const float base_cell_h    = cell_h;

    // Ctrl+MouseWheel zoom on the canvas (like a typical editor).
    // We also adjust scroll so the point under the mouse stays stable.
    // NOTE: We apply the zoom immediately (so sizing updates this frame), but we defer the
    // scroll correction until after the canvas InvisibleButton is created, because the
    // correct "origin" for mouse anchoring is GetItemRectMin() (the actual canvas item rect),
    // not GetCursorScreenPos() (which can drift with child scrolling/scrollbars).
    bool  wheel_zoom_this_frame = false;
    float wheel_zoom_ratio = 1.0f; // ratio between snapped scales (new/old)
    float wheel_pre_scroll_x = 0.0f;
    float wheel_pre_scroll_y = 0.0f;
    ImVec2 wheel_mouse_pos(0.0f, 0.0f);
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.MouseWheel != 0.0f &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
        {
            auto snapped_scale_for_zoom = [&](float zoom) -> float
            {
                // Must match the snapping logic below (snap based on cell_w).
                float snapped_cell_w = std::floor(base_cell_w * zoom + 0.5f);
                if (snapped_cell_w < 1.0f)
                    snapped_cell_w = 1.0f;
                return (base_cell_w > 0.0f) ? (snapped_cell_w / base_cell_w) : 1.0f;
            };

            const float old_zoom = m_zoom;
            const float old_scale = snapped_scale_for_zoom(old_zoom);

            wheel_pre_scroll_x = ImGui::GetScrollX();
            wheel_pre_scroll_y = ImGui::GetScrollY();
            wheel_mouse_pos = io.MousePos;

            const float factor = (io.MouseWheel > 0.0f) ? 1.10f : (1.0f / 1.10f);
            SetZoom(old_zoom * factor);

            const float new_zoom = m_zoom;
            const float new_scale = snapped_scale_for_zoom(new_zoom);
            wheel_zoom_ratio = (old_scale > 0.0f) ? (new_scale / old_scale) : 1.0f;
            wheel_zoom_this_frame = true;
        }
    }

    // Explicit zoom (no auto-fit), but SNAP to the nearest pixel-aligned glyph cell.
    //
    // IMPORTANT: do NOT round width/height independently based on m_zoom.
    // That breaks the font's cell aspect ratio and can create visible seams between glyphs.
    // Instead:
    //  - snap cell_w to integer pixels
    //  - derive a single snapped_scale from that
    //  - compute font size and cell_h from the same snapped_scale
    float snapped_cell_w = std::floor(base_cell_w * m_zoom + 0.5f);
    if (snapped_cell_w < 1.0f)
        snapped_cell_w = 1.0f;
    const float snapped_scale = snapped_cell_w / base_cell_w;

    float scaled_font_size = std::max(1.0f, std::floor(base_font_size * snapped_scale + 0.5f));
    float scaled_cell_w    = snapped_cell_w;
    float scaled_cell_h    = std::floor(base_cell_h * snapped_scale + 0.5f);
    if (scaled_cell_h < 1.0f)
        scaled_cell_h = 1.0f;

    // Expose last aspect for tools/scripts.
    if (scaled_cell_h > 0.0f)
        m_last_cell_aspect = scaled_cell_w / scaled_cell_h;
    else
        m_last_cell_aspect = 1.0f;

    // Capture keyboard events and let the active tool handle them *before* we compute canvas_size,
    // so row growth (typing/enter/wrap) updates ImGui's scroll range immediately.
    //
    // Performance/UX:
    // Historically we began/ended an undo capture every frame, which meant any tool that paints
    // continuously while the mouse is held down would take a full document snapshot once per frame.
    //
    // On large canvases (many rows) that is O(cols*rows) per frame and quickly becomes unusable.
    // Instead, keep a single undo capture open across a mouse-drag "gesture" (mouse capture held),
    // so we snapshot at most once per drag and commit the undo step on mouse release.
    if (!m_undo_capture_active)
        BeginUndoCapture();
    CaptureKeyEvents();
    const int caret_start_row = m_caret_row;
    const int caret_start_col = m_caret_col;
    const bool had_typed_input = !m_typed_queue.empty();
    const bool had_key_input =
        m_key_events.left || m_key_events.right || m_key_events.up || m_key_events.down ||
        m_key_events.home || m_key_events.end || m_key_events.backspace || m_key_events.del ||
        m_key_events.enter;
    if (tool_runner)
    {
        ToolRunScope scope(*this);

        tool_runner(*this, 0); // keyboard phase
    }

    // Keep document large enough for caret after tool run.
    EnsureRows(m_caret_row + 1);

    ImVec2 canvas_size(scaled_cell_w * static_cast<float>(m_columns),
                       scaled_cell_h * static_cast<float>(m_rows));

    // Apply any deferred scroll request now that we have a valid child window.
    // Note: clamp to scrollable bounds using InnerClipRect (what the renderer clips to).
    bool suppress_caret_autoscroll = false;
    if (m_scroll_request_valid)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImRect clip = w ? w->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip.GetWidth();
        const float view_h = clip.GetHeight();
        const float max_x = std::max(0.0f, canvas_size.x - view_w);
        const float max_y = std::max(0.0f, canvas_size.y - view_h);

        float sx = m_scroll_request_x;
        float sy = m_scroll_request_y;
        if (sx < 0.0f) sx = 0.0f;
        if (sy < 0.0f) sy = 0.0f;
        if (sx > max_x) sx = max_x;
        if (sy > max_y) sy = max_y;

        ImGui::SetScrollX(sx);
        ImGui::SetScrollY(sy);

        suppress_caret_autoscroll = true;
        m_scroll_request_valid = false;
    }

    // Capture both left and right mouse buttons so tools/scripts can react to either click+drag.
    ImGui::InvisibleButton(id, canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    origin.x = std::floor(origin.x);
    origin.y = std::floor(origin.y);

    // If we zoomed this frame via Ctrl+MouseWheel, correct scroll so the point under the mouse
    // stays stable in *canvas pixel space*.
    //
    // This must happen AFTER InvisibleButton() so we can use GetItemRectMin() as the true origin.
    if (wheel_zoom_this_frame && wheel_zoom_ratio > 0.0f)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImRect clip = w ? w->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip.GetWidth();
        const float view_h = clip.GetHeight();

        // We'll adjust scroll *now* (after InvisibleButton exists), but that means the
        // screen-space position of the canvas item changes immediately with scroll.
        // If we don't compensate, we'll draw one frame with an origin computed under
        // the old scroll, then the next frame under the new scroll -> visible flicker.
        const float scroll_before_x = ImGui::GetScrollX();
        const float scroll_before_y = ImGui::GetScrollY();

        // Choose anchor point:
        // - prefer the real mouse position if it's inside the visible canvas viewport
        // - otherwise fall back to viewport center (more robust when wheel comes from scrollbars)
        float local_x = (wheel_mouse_pos.x - origin.x);
        float local_y = (wheel_mouse_pos.y - origin.y);
        const bool mouse_in_view =
            (wheel_mouse_pos.x >= clip.Min.x && wheel_mouse_pos.x <= clip.Max.x &&
             wheel_mouse_pos.y >= clip.Min.y && wheel_mouse_pos.y <= clip.Max.y);
        if (!mouse_in_view)
        {
            local_x = view_w * 0.5f;
            local_y = view_h * 0.5f;
        }
        local_x = std::clamp(local_x, 0.0f, std::max(0.0f, view_w));
        local_y = std::clamp(local_y, 0.0f, std::max(0.0f, view_h));

        const float world_x = wheel_pre_scroll_x + local_x;
        const float world_y = wheel_pre_scroll_y + local_y;

        float new_scroll_x = world_x * wheel_zoom_ratio - local_x;
        float new_scroll_y = world_y * wheel_zoom_ratio - local_y;

        // Clamp to scrollable bounds for the new canvas size.
        const float max_x = std::max(0.0f, canvas_size.x - view_w);
        const float max_y = std::max(0.0f, canvas_size.y - view_h);
        if (new_scroll_x < 0.0f) new_scroll_x = 0.0f;
        if (new_scroll_y < 0.0f) new_scroll_y = 0.0f;
        if (new_scroll_x > max_x) new_scroll_x = max_x;
        if (new_scroll_y > max_y) new_scroll_y = max_y;

        ImGui::SetScrollX(new_scroll_x);
        ImGui::SetScrollY(new_scroll_y);

        // Compensate origin for the scroll we just applied so drawing uses the correct
        // screen-space origin for this same frame.
        const float dx = new_scroll_x - scroll_before_x;
        const float dy = new_scroll_y - scroll_before_y;
        origin.x -= dx;
        origin.y -= dy;
        origin.x = std::floor(origin.x);
        origin.y = std::floor(origin.y);

        suppress_caret_autoscroll = true; // avoid “fight” between zoom anchoring and caret-follow
    }

    // Base canvas background is NOT theme-driven; it's a fixed black/white fill so
    // the editing "paper" stays consistent regardless of UI skin.
    {
        const ImU32 bg = m_canvas_bg_white ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
        draw_list->AddRectFilled(origin,
                                 ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y),
                                 ApplyCurrentStyleAlpha(bg));
    }

    // Focus rules:
    // - click inside the grid to focus
    // - click elsewhere *within the same canvas window* to defocus
    //
    // IMPORTANT: don't defocus on global UI clicks (e.g. main menu bar) so menu actions
    // like File/Save and Edit/Undo can still target the active canvas.
    m_focus_gained = false; // transient per-frame
    const bool was_focused = m_has_focus;
    const bool any_click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (ImGui::IsItemHovered() && any_click)
        m_has_focus = true;
    else if (!ImGui::IsItemHovered() && any_click)
    {
        // Only clear focus if the click was in this window (or its child windows).
        // If the click was outside (e.g. main menu bar, another window), keep focus.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            m_has_focus = false;
    }
    if (!was_focused && m_has_focus)
        m_focus_gained = true;

    HandleMouseInteraction(origin, scaled_cell_w, scaled_cell_h);

    // Mouse phase: tools can react to cursor state for this frame.
    if (tool_runner)
    {
        ToolRunScope scope(*this);

        tool_runner(*this, 1);
    }

    // End undo capture unless the user is in an active mouse gesture that may continue mutating
    // the canvas across multiple frames (e.g. pencil drag, selection move).
    //
    // Note: HandleMouseInteraction updates m_mouse_capture using the current ImGui mouse state,
    // including releases that happen outside the item while captured.
    const bool keep_undo_open_for_mouse_gesture = m_mouse_capture || m_move.active;
    if (!keep_undo_open_for_mouse_gesture)
        EndUndoCapture();

    // Keep cursor visible when navigating.
    //
    // Important: only auto-scroll to caret when there was keyboard/text input this frame.
    // This prevents "snap-back" after mouse-driven scrolling/panning (e.g. preview minimap drag),
    // and avoids fighting tools that adjust the caret during mouse painting.
    const bool caret_moved = (m_caret_row != caret_start_row) || (m_caret_col != caret_start_col);
    const bool mouse_painting = m_cursor_valid && (m_cursor_left_down || m_cursor_right_down);
    const bool should_follow_caret = had_key_input || had_typed_input || (caret_moved && mouse_painting);
    if (m_has_focus && m_follow_caret && !suppress_caret_autoscroll && should_follow_caret)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect clip_rect = window ? window->InnerClipRect : ImRect(0, 0, 0, 0);
        const float view_w = clip_rect.GetWidth();
        const float view_h = clip_rect.GetHeight();

        const float scroll_x = ImGui::GetScrollX();
        const float scroll_y = ImGui::GetScrollY();

        const float cursor_x0 = static_cast<float>(m_caret_col) * scaled_cell_w;
        const float cursor_x1 = cursor_x0 + scaled_cell_w;
        const float cursor_y0 = static_cast<float>(m_caret_row) * scaled_cell_h;
        const float cursor_y1 = cursor_y0 + scaled_cell_h;

        if (cursor_x0 < scroll_x)
            ImGui::SetScrollX(cursor_x0);
        else if (cursor_x1 > scroll_x + view_w)
            ImGui::SetScrollX(cursor_x1 - view_w);

        if (cursor_y0 < scroll_y)
            ImGui::SetScrollY(cursor_y0);
        else if (cursor_y1 > scroll_y + view_h)
            ImGui::SetScrollY(cursor_y1 - view_h);
    }

    DrawVisibleCells(draw_list, origin, scaled_cell_w, scaled_cell_h, scaled_font_size);
    DrawMirrorAxisOverlay(draw_list, origin, scaled_cell_w, scaled_cell_h, canvas_size);
    DrawActiveLayerBoundsOverlay(draw_list, origin, scaled_cell_w, scaled_cell_h);
    DrawSelectionOverlay(draw_list, origin, scaled_cell_w, scaled_cell_h, scaled_font_size);

    // Capture last viewport metrics for minimap/preview. Do this at the very end so any
    // caret auto-scroll or scroll requests are reflected.
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImRect clip_rect = window ? window->InnerClipRect : ImRect(0, 0, 0, 0);
        m_last_view.valid = true;
        m_last_view.columns = m_columns;
        m_last_view.rows = m_rows;
        m_last_view.zoom = m_zoom;
        m_last_view.base_cell_w = base_cell_w;
        m_last_view.base_cell_h = base_cell_h;
        m_last_view.cell_w = scaled_cell_w;
        m_last_view.cell_h = scaled_cell_h;
        m_last_view.canvas_w = canvas_size.x;
        m_last_view.canvas_h = canvas_size.y;
        m_last_view.view_w = clip_rect.GetWidth();
        m_last_view.view_h = clip_rect.GetHeight();
        m_last_view.scroll_x = ImGui::GetScrollX();
        m_last_view.scroll_y = ImGui::GetScrollY();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void AnsiCanvas::DrawMirrorAxisOverlay(ImDrawList* draw_list,
                                      const ImVec2& origin,
                                      float cell_w,
                                      float /*cell_h*/,
                                      const ImVec2& canvas_size)
{
    if (!draw_list || !m_mirror_mode || m_columns <= 0)
        return;

    // Same hue as selection border (see DrawSelectionOverlay) but more subtle.
    const ImU32 col = ImGui::GetColorU32(ImVec4(0.15f, 0.75f, 1.0f, 0.35f));

    // Axis is at the center of the grid in "cell units": columns/2.
    float x = origin.x + cell_w * (static_cast<float>(m_columns) * 0.5f);
    x = std::floor(x) + 0.5f; // pixel align like selection border

    const ImVec2 p0(x, origin.y);
    const ImVec2 p1(x, origin.y + canvas_size.y);
    draw_list->AddLine(p0, p1, col, 2.0f);
}

void AnsiCanvas::DrawActiveLayerBoundsOverlay(ImDrawList* draw_list,
                                             const ImVec2& origin,
                                             float cell_w,
                                             float cell_h)
{
    if (!draw_list)
        return;
    if (m_columns <= 0 || m_rows <= 0)
        return;
    if (m_active_layer < 0 || m_active_layer >= (int)m_layers.size())
        return;

    const Layer& layer = m_layers[(size_t)m_active_layer];
    if (layer.offset_x == 0 && layer.offset_y == 0)
        return;

    // Subtle light-grey outline (only when the layer is offset, per UX request).
    const ImU32 col = ImGui::GetColorU32(ImVec4(0.85f, 0.85f, 0.85f, 0.35f));

    const int x0 = layer.offset_x;
    const int y0 = layer.offset_y;
    const int x1 = x0 + m_columns;
    const int y1 = y0 + m_rows;

    ImVec2 p0(origin.x + (float)x0 * cell_w, origin.y + (float)y0 * cell_h);
    ImVec2 p1(origin.x + (float)x1 * cell_w, origin.y + (float)y1 * cell_h);
    // Pixel-align like selection border for crisp 1px lines.
    p0.x = std::floor(p0.x) + 0.5f;
    p0.y = std::floor(p0.y) + 0.5f;
    p1.x = std::floor(p1.x) - 0.5f;
    p1.y = std::floor(p1.y) - 0.5f;
    draw_list->AddRect(p0, p1, col, 0.0f, 0, 1.0f);
}

// ---- end inlined from canvas_render.inc ----


