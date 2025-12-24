#include "ui/glyph_preview.h"

#include "imgui.h"

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/glyph_resolve.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
static std::string EncodeCodePointUtf8(char32_t cp)
{
    // Minimal UTF-8 encoder for a single Unicode scalar value.
    const std::uint32_t u = (std::uint32_t)cp;
    if (u == 0 || u > 0x10FFFFu || (u >= 0xD800u && u <= 0xDFFFu))
        return {};

    char out[4];
    size_t n = 0;
    if (u <= 0x7Fu)
    {
        out[0] = (char)u;
        n = 1;
    }
    else if (u <= 0x7FFu)
    {
        out[0] = (char)(0xC0u | (u >> 6));
        out[1] = (char)(0x80u | (u & 0x3Fu));
        n = 2;
    }
    else if (u <= 0xFFFFu)
    {
        out[0] = (char)(0xE0u | (u >> 12));
        out[1] = (char)(0x80u | ((u >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (u & 0x3Fu));
        n = 3;
    }
    else
    {
        out[0] = (char)(0xF0u | (u >> 18));
        out[1] = (char)(0x80u | ((u >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((u >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (u & 0x3Fu));
        n = 4;
    }
    return std::string(out, out + n);
}

static bool CalcTightGlyphBounds(ImFont* font, float font_px, unsigned int codepoint,
                                 ImVec2& out_min, ImVec2& out_max, float& out_advance_x)
{
    out_min = ImVec2(FLT_MAX, FLT_MAX);
    out_max = ImVec2(-FLT_MAX, -FLT_MAX);
    out_advance_x = 0.0f;
    if (!font || font_px <= 0.0f)
        return false;

    // ImGui 1.92+: metrics live on ImFontBaked (per requested size).
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
    ImFontBaked* baked = font->GetFontBaked(font_px);
    if (!baked)
        return false;

    unsigned int cp = codepoint;
    if (cp > (unsigned int)IM_UNICODE_CODEPOINT_MAX)
        cp = (unsigned int)font->FallbackChar;

    // Use FindGlyph() (with fallback) so bbox remains stable even when glyph isn't present.
    const ImFontGlyph* g = baked->FindGlyph((ImWchar)cp);
    if (!g)
        return false;

    out_min = ImVec2(g->X0, g->Y0);
    out_max = ImVec2(g->X1, g->Y1);
    out_advance_x = g->AdvanceX;
    return true;
#else
    // Older ImGui: metrics live on ImFont with a fixed FontSize and FindGlyph().
    const float scale = font_px / font->FontSize;
    unsigned int cp = codepoint;
    if (cp > (unsigned int)IM_UNICODE_CODEPOINT_MAX)
        cp = (unsigned int)font->FallbackChar;
    const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
    if (!g)
        return false;
    out_min = ImVec2(g->X0 * scale, g->Y0 * scale);
    out_max = ImVec2(g->X1 * scale, g->Y1 * scale);
    out_advance_x = g->AdvanceX * scale;
    return true;
#endif
}

static inline float SnapPx(float v)
{
    // Round-to-nearest pixel to avoid fuzzy text at fractional coords.
    return std::floor(v + 0.5f);
}
} // namespace

void DrawGlyphPreview(ImDrawList* dl,
                      const ImVec2& p0,
                      float cell_w,
                      float cell_h,
                      phos::GlyphId glyph,
                      const AnsiCanvas* canvas,
                      std::uint32_t fg_col)
{
    if (!dl)
        return;
    if (cell_w <= 0.0f || cell_h <= 0.0f)
        return;

    if (phos::glyph::IsBlank(glyph))
        return;

    const char32_t cp_rep = phos::glyph::ToUnicodeRepresentative(glyph);

    // No canvas -> just draw text with the UI font.
    if (!canvas)
    {
        const std::string s = EncodeCodePointUtf8(cp_rep);
        if (!s.empty())
        {
            ImFont* font = ImGui::GetFont();
            const float cell = std::min(cell_w, cell_h);
            float font_px = std::max(6.0f, cell * 0.85f);

            // Prefer tight glyph bounds for optical centering (esp. emoji + symbols).
            ImVec2 bmin, bmax;
            float adv = 0.0f;
            bool have_bounds = CalcTightGlyphBounds(font, font_px, (unsigned int)cp_rep, bmin, bmax, adv);
            const float max_dim = cell * 0.92f;
            if (have_bounds)
            {
                const ImVec2 bsz(bmax.x - bmin.x, bmax.y - bmin.y);
                if (bsz.x > 1.0f && bsz.x > max_dim) font_px *= (max_dim / bsz.x);
                if (bsz.y > 1.0f && bsz.y > max_dim) font_px *= (max_dim / bsz.y);
            }
            else
            {
                // Fallback shrink based on CalcTextSizeA if bounds aren't available.
                ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
                if (ts.x > 1.0f && ts.x > max_dim) font_px *= (max_dim / ts.x);
                if (ts.y > 1.0f && ts.y > max_dim) font_px *= (max_dim / ts.y);
            }

            have_bounds = CalcTightGlyphBounds(font, font_px, (unsigned int)cp_rep, bmin, bmax, adv);
            if (have_bounds)
            {
                const ImVec2 bsz(bmax.x - bmin.x, bmax.y - bmin.y);
                ImVec2 tp(p0.x + (cell_w - bsz.x) * 0.5f - bmin.x,
                          p0.y + (cell_h - bsz.y) * 0.5f - bmin.y);
                tp.x = SnapPx(tp.x);
                tp.y = SnapPx(tp.y);
                dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
            }
            else
            {
                // Last resort: line-height centering.
                ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
                ImVec2 tp(p0.x + (cell_w - ts.x) * 0.5f, p0.y + (cell_h - ts.y) * 0.5f);
                tp.x = SnapPx(tp.x);
                tp.y = SnapPx(tp.y);
                dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
            }
        }
        return;
    }

    const fonts::FontInfo& finfo = fonts::Get(canvas->GetFontId());
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas->GetEmbeddedFont();
    const bool embedded_font_ok =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font =
        embedded_font_ok ||
        (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    if (!bitmap_font)
    {
        const std::string s = EncodeCodePointUtf8(cp_rep);
        if (s.empty())
            return;

        ImFont* font = ImGui::GetFont();
        const float cell = std::min(cell_w, cell_h);
        float font_px = std::max(6.0f, cell * 0.85f);
        const float max_dim = cell * 0.92f;

        ImVec2 bmin, bmax;
        float adv = 0.0f;
        bool have_bounds = CalcTightGlyphBounds(font, font_px, (unsigned int)cp_rep, bmin, bmax, adv);
        if (have_bounds)
        {
            const ImVec2 bsz(bmax.x - bmin.x, bmax.y - bmin.y);
            if (bsz.x > 1.0f && bsz.x > max_dim) font_px *= (max_dim / bsz.x);
            if (bsz.y > 1.0f && bsz.y > max_dim) font_px *= (max_dim / bsz.y);
        }
        else
        {
            ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
            if (ts.x > 1.0f && ts.x > max_dim) font_px *= (max_dim / ts.x);
            if (ts.y > 1.0f && ts.y > max_dim) font_px *= (max_dim / ts.y);
        }

        have_bounds = CalcTightGlyphBounds(font, font_px, (unsigned int)cp_rep, bmin, bmax, adv);
        if (have_bounds)
        {
            const ImVec2 bsz(bmax.x - bmin.x, bmax.y - bmin.y);
            ImVec2 tp(p0.x + (cell_w - bsz.x) * 0.5f - bmin.x,
                      p0.y + (cell_h - bsz.y) * 0.5f - bmin.y);
            tp.x = SnapPx(tp.x);
            tp.y = SnapPx(tp.y);
            dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
        }
        else
        {
            ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
            ImVec2 tp(p0.x + (cell_w - ts.x) * 0.5f, p0.y + (cell_h - ts.y) * 0.5f);
            tp.x = SnapPx(tp.x);
            tp.y = SnapPx(tp.y);
            dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
        }
        return;
    }

    // Prefer the canvas's bitmap glyph atlas (if available) so previews match the main renderer.
    if (canvas && canvas->GetBitmapGlyphAtlasProvider())
    {
        AnsiCanvas::BitmapGlyphAtlasView atlas{};
        if (canvas->GetBitmapGlyphAtlasProvider()->GetBitmapGlyphAtlas(*canvas, atlas) &&
            atlas.texture_id != nullptr && atlas.atlas_w > 0 && atlas.atlas_h > 0 &&
            atlas.cell_w > 0 && atlas.cell_h > 0 && atlas.tile_w > 0 && atlas.tile_h > 0 &&
            atlas.cols > 0 && atlas.rows > 0 && atlas.glyph_count > 0)
        {
            // Resolve glyph index (same rules as the canvas renderer).
            int glyph_cell_w = finfo.cell_w;
            int glyph_cell_h = finfo.cell_h;
            bool vga_dup = finfo.vga_9col_dup;

            if (embedded_font_ok)
            {
                glyph_cell_w = ef->cell_w;
                glyph_cell_h = ef->cell_h;
                vga_dup = ef->vga_9col_dup;
            }
            (void)glyph_cell_w;
            (void)glyph_cell_h;
            (void)vga_dup;
            const std::uint16_t glyph_index = phos::glyph::ResolveBitmapGlyph(finfo, ef, glyph).glyph_index;

            const int gi = (int)glyph_index;
            if (gi >= 0 && gi < atlas.glyph_count)
            {
                const int variant = 0; // preview shows normal only (attrs are not modeled here)
                const int tile_x = gi % atlas.cols;
                const int tile_y = gi / atlas.cols;
                if (tile_y >= 0 && tile_y < atlas.rows)
                {
                    const int px0 = tile_x * atlas.tile_w + atlas.pad;
                    const int py0 = (variant * atlas.rows + tile_y) * atlas.tile_h + atlas.pad;
                    const int px1 = px0 + atlas.cell_w;
                    const int py1 = py0 + atlas.cell_h;
                    float u0 = (float)px0 / (float)atlas.atlas_w;
                    float v0 = (float)py0 / (float)atlas.atlas_h;
                    float u1 = (float)px1 / (float)atlas.atlas_w;
                    float v1 = (float)py1 / (float)atlas.atlas_h;
                    // Match the main canvas renderer: for NEAREST sampling, map to texel edges (not centers).
                    // Nudge max UV inward by 1 ULP to avoid bleeding into adjacent atlas tiles.
                    u1 = std::nextafter(u1, u0);
                    v1 = std::nextafter(v1, v0);
                    const ImVec2 uv0(u0, v0);
                    const ImVec2 uv1(u1, v1);
                    dl->AddImage((ImTextureID)atlas.texture_id,
                                 p0, ImVec2(p0.x + cell_w, p0.y + cell_h),
                                 uv0, uv1, (ImU32)fg_col);
                    return;
                }
            }
        }
    }

    // Bitmap/embedded path: mirror the canvas renderer's glyph index resolution rules.
    int glyph_cell_w = finfo.cell_w;
    int glyph_cell_h = finfo.cell_h;
    bool vga_dup = finfo.vga_9col_dup;

    if (embedded_font_ok)
    {
        glyph_cell_w = ef->cell_w;
        glyph_cell_h = ef->cell_h;
        vga_dup = ef->vga_9col_dup;
    }
    const std::uint16_t glyph_index = phos::glyph::ResolveBitmapGlyph(finfo, ef, glyph).glyph_index;

    auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
    {
        if (embedded_font_ok)
        {
            if (gi >= (std::uint16_t)ef->glyph_count) return 0;
            if (yy < 0 || yy >= ef->cell_h) return 0;
            return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
        }
        return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
    };

    const float px_w = cell_w / (float)std::max(1, glyph_cell_w);
    const float px_h = cell_h / (float)std::max(1, glyph_cell_h);
    const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);

    for (int yy = 0; yy < glyph_cell_h; ++yy)
    {
        const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
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
                const float x0 = p0.x + (float)run_start * px_w;
                const float x1 = p0.x + (float)run_end * px_w;
                const float y0 = p0.y + (float)yy * px_h;
                const float y1 = p0.y + (float)(yy + 1) * px_h;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), (ImU32)fg_col);
                run_start = -1;
            }
        }
    }
}


