#include "core/canvas_rasterizer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace canvas_rasterizer
{
namespace
{
static inline void UnpackImGui(ImU32 c, int& r, int& g, int& b, int& a)
{
    r = (int)(c & 0xFF);
    g = (int)((c >> 8) & 0xFF);
    b = (int)((c >> 16) & 0xFF);
    a = (int)((c >> 24) & 0xFF);
}

static inline ImU32 PackImGui(int r, int g, int b, int a)
{
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    a = std::clamp(a, 0, 255);
    return (ImU32)((a << 24) | (b << 16) | (g << 8) | (r));
}

static inline void BlendOver(ImU32& dst, ImU32 src)
{
    int dr, dg, db, da;
    int sr, sg, sb, sa;
    UnpackImGui(dst, dr, dg, db, da);
    UnpackImGui(src, sr, sg, sb, sa);

    const float a = (float)sa / 255.0f;
    const int out_a = (int)std::lround((double)da + (double)(255 - da) * (double)a);
    const int out_r = (int)std::lround((double)dr + ((double)sr - (double)dr) * (double)a);
    const int out_g = (int)std::lround((double)dg + ((double)sg - (double)dg) * (double)a);
    const int out_b = (int)std::lround((double)db + ((double)sb - (double)db) * (double)a);
    dst = PackImGui(out_r, out_g, out_b, out_a);
}
} // namespace

bool RasterizeCompositeToRgba32(const AnsiCanvas& canvas,
                               std::vector<std::uint8_t>& out_rgba,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const Options& opt)
{
    err.clear();
    out_rgba.clear();
    out_w = 0;
    out_h = 0;

    const int cols = canvas.GetColumns();
    const int rows = canvas.GetRows();
    if (cols <= 0 || rows <= 0)
    {
        err = "Invalid canvas dimensions.";
        return false;
    }

    const int scale = std::clamp(opt.scale, 1, 16);

    // Determine base cell size from the active ImGui font. Unscii 16px is expected to be 8x16.
    ImFont* font = ImGui::GetFont();
    if (!font)
    {
        err = "No active ImGui font.";
        return false;
    }
    ImFontAtlas* atlas = font->OwnerAtlas ? font->OwnerAtlas : ImGui::GetIO().Fonts;
    if (!atlas)
    {
        err = "No ImGui font atlas.";
        return false;
    }

    unsigned char* atlas_rgba = nullptr;
    int atlas_w = 0, atlas_h = 0;
    atlas->GetTexDataAsRGBA32(&atlas_rgba, &atlas_w, &atlas_h);
    if (!atlas_rgba || atlas_w <= 0 || atlas_h <= 0)
    {
        err = "ImGui font atlas has no RGBA texture data.";
        return false;
    }

    // Use current font size metrics, rounded to integer pixels for deterministic "text-mode" output.
    const float font_size = ImGui::GetFontSize();
    const float cell_w_f = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
    const float cell_h_f = font_size;
    const int cell_w = std::max(1, (int)std::lround((double)cell_w_f));
    const int cell_h = std::max(1, (int)std::lround((double)cell_h_f));

    out_w = cols * cell_w * scale;
    out_h = rows * cell_h * scale;
    if (out_w <= 0 || out_h <= 0)
    {
        err = "Invalid output dimensions.";
        return false;
    }

    out_rgba.assign((size_t)out_w * (size_t)out_h * 4u, 0u);

    const ImU32 paper = canvas.IsCanvasBackgroundWhite() ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
    const ImU32 default_fg = canvas.IsCanvasBackgroundWhite() ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

    auto set_px = [&](int x, int y, ImU32 c)
    {
        if ((unsigned)x >= (unsigned)out_w || (unsigned)y >= (unsigned)out_h)
            return;
        const size_t i = ((size_t)y * (size_t)out_w + (size_t)x) * 4u;
        out_rgba[i + 0] = (std::uint8_t)(c & 0xFF);
        out_rgba[i + 1] = (std::uint8_t)((c >> 8) & 0xFF);
        out_rgba[i + 2] = (std::uint8_t)((c >> 16) & 0xFF);
        out_rgba[i + 3] = (std::uint8_t)((c >> 24) & 0xFF);
    };

    auto get_px = [&](int x, int y) -> ImU32
    {
        const size_t i = ((size_t)y * (size_t)out_w + (size_t)x) * 4u;
        return PackImGui(out_rgba[i + 0], out_rgba[i + 1], out_rgba[i + 2], out_rgba[i + 3]);
    };

    // Pre-fill with paper if we are not emitting transparent unset backgrounds.
    if (!opt.transparent_unset_bg)
    {
        for (int y = 0; y < out_h; ++y)
            for (int x = 0; x < out_w; ++x)
                set_px(x, y, paper);
    }

    // Rasterize per-cell, using glyph alpha from the atlas.
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            char32_t cp = U' ';
            AnsiCanvas::Color32 fg = 0;
            AnsiCanvas::Color32 bg = 0;
            (void)canvas.GetCompositeCellPublic(row, col, cp, fg, bg);

            const ImU32 fg_col = (fg != 0) ? (ImU32)fg : default_fg;
            ImU32 bg_col = paper;
            if (bg != 0)
                bg_col = (ImU32)bg;
            else if (opt.transparent_unset_bg)
                bg_col = IM_COL32(0, 0, 0, 0);

            const int cell_x0 = col * cell_w * scale;
            const int cell_y0 = row * cell_h * scale;

            // Paint background for the cell (including transparent bg if requested).
            for (int yy = 0; yy < cell_h * scale; ++yy)
            {
                for (int xx = 0; xx < cell_w * scale; ++xx)
                {
                    set_px(cell_x0 + xx, cell_y0 + yy, bg_col);
                }
            }

            if (cp == U' ')
                continue;

            // Use Dear ImGui baked font data if possible; otherwise best-effort fall back.
            ImFontBaked* baked = ImGui::GetFontBaked();
            if (!baked)
            {
                const float bake_size = (font->LegacySize > 0.0f) ? font->LegacySize : 16.0f;
                baked = const_cast<ImFont*>(font)->GetFontBaked(bake_size);
            }

            const ImFontGlyph* g = baked ? baked->FindGlyphNoFallback((ImWchar)cp) : nullptr;
            if (!g)
                continue;

            const int gx0 = std::clamp((int)std::floor(g->U0 * (float)atlas_w), 0, atlas_w);
            const int gy0 = std::clamp((int)std::floor(g->V0 * (float)atlas_h), 0, atlas_h);
            const int gx1 = std::clamp((int)std::ceil(g->U1 * (float)atlas_w), 0, atlas_w);
            const int gy1 = std::clamp((int)std::ceil(g->V1 * (float)atlas_h), 0, atlas_h);
            const int gw = std::max(0, gx1 - gx0);
            const int gh = std::max(0, gy1 - gy0);
            if (gw <= 0 || gh <= 0)
                continue;

            // Center glyph rect within the cell (Unscii should already match 8x16, so this is typically 0,0).
            const int off_x = (cell_w - gw) / 2;
            const int off_y = (cell_h - gh) / 2;

            for (int sy = 0; sy < gh; ++sy)
            {
                for (int sx = 0; sx < gw; ++sx)
                {
                    const size_t abase = (size_t)((gy0 + sy) * atlas_w + (gx0 + sx)) * 4u;
                    const std::uint8_t a8 = atlas_rgba[abase + 3];
                    if (a8 == 0)
                        continue;

                    // Source pixel alpha -> premultiply into fg color.
                    int fr, fg_, fb, fa;
                    UnpackImGui(fg_col, fr, fg_, fb, fa);
                    const int src_a = (int)a8; // atlas alpha is coverage
                    const ImU32 src = PackImGui(fr, fg_, fb, src_a);

                    const int dx0 = cell_x0 + (off_x + sx) * scale;
                    const int dy0 = cell_y0 + (off_y + sy) * scale;

                    for (int yy = 0; yy < scale; ++yy)
                    {
                        for (int xx = 0; xx < scale; ++xx)
                        {
                            const int dx = dx0 + xx;
                            const int dy = dy0 + yy;
                            if ((unsigned)dx >= (unsigned)out_w || (unsigned)dy >= (unsigned)out_h)
                                continue;
                            ImU32 dst = get_px(dx, dy);
                            BlendOver(dst, src);
                            set_px(dx, dy, dst);
                        }
                    }
                }
            }
        }
    }

    return true;
}
} // namespace canvas_rasterizer


