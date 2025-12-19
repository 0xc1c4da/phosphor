#include "ui/glyph_preview.h"

#include "imgui.h"

#include "core/canvas.h"
#include "core/fonts.h"

#include <algorithm>
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
} // namespace

void DrawGlyphPreview(ImDrawList* dl,
                      const ImVec2& p0,
                      float cell_w,
                      float cell_h,
                      char32_t cp,
                      const AnsiCanvas* canvas,
                      std::uint32_t fg_col)
{
    if (!dl)
        return;
    if (cell_w <= 0.0f || cell_h <= 0.0f)
        return;

    // No canvas -> just draw text with the UI font.
    if (!canvas)
    {
        const std::string s = EncodeCodePointUtf8(cp);
        if (!s.empty())
        {
            ImFont* font = ImGui::GetFont();
            const float cell = std::min(cell_w, cell_h);
            float font_px = std::max(6.0f, cell * 0.85f);
            ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
            const float max_dim = cell * 0.92f;
            if (ts.x > max_dim && ts.x > 0.0f) font_px *= (max_dim / ts.x);
            if (ts.y > max_dim && ts.y > 0.0f) font_px *= (max_dim / ts.y);
            ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
            const ImVec2 tp(p0.x + (cell_w - ts.x) * 0.5f,
                            p0.y + (cell_h - ts.y) * 0.5f);
            dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
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
        const std::string s = EncodeCodePointUtf8(cp);
        if (s.empty())
            return;

        ImFont* font = ImGui::GetFont();
        const float cell = std::min(cell_w, cell_h);
        float font_px = std::max(6.0f, cell * 0.85f);
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
        const float max_dim = cell * 0.92f;
        if (ts.x > max_dim && ts.x > 0.0f) font_px *= (max_dim / ts.x);
        if (ts.y > max_dim && ts.y > 0.0f) font_px *= (max_dim / ts.y);
        ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, s.c_str(), nullptr);
        const ImVec2 tp(p0.x + (cell_w - ts.x) * 0.5f,
                        p0.y + (cell_h - ts.y) * 0.5f);
        dl->AddText(font, font_px, tp, (ImU32)fg_col, s.c_str(), nullptr);
        return;
    }

    // Bitmap/embedded path: mirror the canvas renderer's glyph index resolution rules.
    int glyph_cell_w = finfo.cell_w;
    int glyph_cell_h = finfo.cell_h;
    bool vga_dup = finfo.vga_9col_dup;

    std::uint16_t glyph_index = 0;
    if (embedded_font_ok)
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
            std::uint8_t b = 0;
            if (fonts::UnicodeToCp437Byte(cp, b))
                glyph_index = (std::uint16_t)b;
            else
                glyph_index = (std::uint16_t)'?';
        }
    }
    else
    {
        std::uint8_t b = 0;
        if (!fonts::UnicodeToCp437Byte(cp, b))
        {
            std::uint8_t q = 0;
            b = (fonts::UnicodeToCp437Byte(U'?', q)) ? q : (std::uint8_t)' ';
        }
        glyph_index = (std::uint16_t)b;
    }

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


