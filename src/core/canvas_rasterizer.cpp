#include "core/canvas_rasterizer.h"

#include "core/fonts.h"

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

    // Straight alpha "source over" blend.
    const double sA = (double)sa / 255.0;
    const double dA = (double)da / 255.0;
    const double oA = sA + dA * (1.0 - sA);
    if (oA <= 0.0)
    {
        dst = 0;
        return;
    }

    // Compute in premultiplied space, then unpremultiply.
    const double sR = (double)sr * sA;
    const double sG = (double)sg * sA;
    const double sB = (double)sb * sA;
    const double dR = (double)dr * dA;
    const double dG = (double)dg * dA;
    const double dB = (double)db * dA;

    const double oR = (sR + dR * (1.0 - sA)) / oA;
    const double oG = (sG + dG * (1.0 - sA)) / oA;
    const double oB = (sB + dB * (1.0 - sA)) / oA;
    const int out_a = (int)std::lround(oA * 255.0);
    const int out_r = (int)std::lround(oR);
    const int out_g = (int)std::lround(oG);
    const int out_b = (int)std::lround(oB);
    dst = PackImGui(out_r, out_g, out_b, out_a);
}
} // namespace

bool ComputeCompositeRasterSize(const AnsiCanvas& canvas,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const Options& opt)
{
    err.clear();
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

    const fonts::FontInfo& finfo = fonts::Get(canvas.GetFontId());
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    int cell_w = 0;
    int cell_h = 0;
    if (bitmap_font)
    {
        if (embedded_font)
        {
            cell_w = std::max(1, ef->cell_w);
            cell_h = std::max(1, ef->cell_h);
        }
        else
        {
            cell_w = std::max(1, finfo.cell_w);
            cell_h = std::max(1, finfo.cell_h);
        }
    }
    else
    {
        ImFont* font = ImGui::GetFont();
        if (!font)
        {
            err = "No active ImGui font.";
            return false;
        }
        const float base_font_size = ImGui::GetFontSize();
        const float cell_w_f = font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
        const float cell_h_f = base_font_size;
        cell_w = std::max(1, (int)std::lround((double)cell_w_f));
        cell_h = std::max(1, (int)std::lround((double)cell_h_f));
    }

    out_w = cols * cell_w * scale;
    out_h = rows * cell_h * scale;
    if (out_w <= 0 || out_h <= 0)
    {
        err = "Invalid output dimensions.";
        return false;
    }
    return true;
}

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

    const fonts::FontInfo& finfo = fonts::Get(canvas.GetFontId());
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Determine base cell size.
    // - Unscii / ImGuiAtlas fonts: derive from the active ImGui font.
    // - Bitmap fonts: use the font's textmode metrics directly (ImGui not required).
    ImFont* font = nullptr;
    float base_font_size = 0.0f;
    int cell_w = 0;
    int cell_h = 0;
    if (bitmap_font)
    {
        if (embedded_font)
        {
            cell_w = std::max(1, ef->cell_w);
            cell_h = std::max(1, ef->cell_h);
        }
        else
        {
            cell_w = std::max(1, finfo.cell_w);
            cell_h = std::max(1, finfo.cell_h);
        }
    }
    else
    {
        font = ImGui::GetFont();
        if (!font)
        {
            err = "No active ImGui font.";
            return false;
        }
        base_font_size = ImGui::GetFontSize();
        const float cell_w_f = font->CalcTextSizeA(base_font_size, FLT_MAX, 0.0f, "M", "M" + 1).x;
        const float cell_h_f = base_font_size;
        cell_w = std::max(1, (int)std::lround((double)cell_w_f));
        cell_h = std::max(1, (int)std::lround((double)cell_h_f));
    }

    // ImGui atlas resources (only needed for ImGuiAtlas font rendering).
    unsigned char* atlas_rgba = nullptr;
    int atlas_w = 0, atlas_h = 0;
    ImFontBaked* baked = nullptr;
    ImFontAtlas* atlas = nullptr;
    if (!bitmap_font)
    {
        atlas = font->OwnerAtlas ? font->OwnerAtlas : ImGui::GetIO().Fonts;
        if (!atlas)
        {
            err = "No ImGui font atlas.";
            return false;
        }
        atlas->GetTexDataAsRGBA32(&atlas_rgba, &atlas_w, &atlas_h);
        if (!atlas_rgba || atlas_w <= 0 || atlas_h <= 0)
        {
            err = "ImGui font atlas has no RGBA texture data.";
            return false;
        }

        baked = ImGui::GetFontBaked();
        if (!baked)
        {
            const float bake_size = (font->LegacySize > 0.0f) ? font->LegacySize : 16.0f;
            baked = const_cast<ImFont*>(font)->GetFontBaked(bake_size);
        }
    }

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

    // Rasterize per-cell.
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

            if (!bitmap_font)
            {
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

                    if (cp >= AnsiCanvas::kEmbeddedGlyphBase &&
                        cp < (AnsiCanvas::kEmbeddedGlyphBase + (char32_t)ef->glyph_count))
                    {
                        glyph_index = (std::uint16_t)(cp - AnsiCanvas::kEmbeddedGlyphBase);
                    }
                    else
                    {
                        // Best-effort: treat as CP437-ordered.
                        std::uint16_t gi = 0;
                        if (fonts::UnicodeToGlyphIndex(finfo.id, cp, gi))
                            glyph_index = gi;
                        else
                            glyph_index = (std::uint16_t)'?';
                    }
                }
                else
                {
                    if (!fonts::UnicodeToGlyphIndex(finfo.id, cp, glyph_index))
                    {
                        if (!fonts::UnicodeToGlyphIndex(finfo.id, U'?', glyph_index))
                            glyph_index = (std::uint16_t)' ';
                    }
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

                const std::uint8_t glyph = (std::uint8_t)(glyph_index & 0xFFu);

                const int px_w = std::max(1, (cell_w * scale) / std::max(1, glyph_cell_w));
                const int px_h = std::max(1, (cell_h * scale) / std::max(1, glyph_cell_h));

                // Render each row as runs of set bits.
                for (int yy = 0; yy < glyph_cell_h; ++yy)
                {
                    const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
                    int run_start = -1;
                    auto bit_set = [&](int x) -> bool
                    {
                        if (x < 8)
                            return (bits & (std::uint8_t)(0x80u >> x)) != 0;
                        if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph >= 192 && glyph <= 223)
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
                            const int dx0 = cell_x0 + run_start * px_w;
                            const int dx1 = cell_x0 + run_end * px_w;
                            const int dy0 = cell_y0 + yy * px_h;
                            const int dy1 = cell_y0 + (yy + 1) * px_h;
                            for (int y = dy0; y < dy1; ++y)
                                for (int x = dx0; x < dx1; ++x)
                                    set_px(x, y, fg_col);
                            run_start = -1;
                        }
                    }
                }
            }
        }
    }

    return true;
}
} // namespace canvas_rasterizer


