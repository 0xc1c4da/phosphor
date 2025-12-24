#include "core/deform/glyph_mask_cache.h"

#include "core/fonts.h"
#include "core/glyph_resolve.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace deform
{
namespace
{
static inline std::uint64_t Hash64(std::uint64_t x)
{
    // splitmix64
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static inline std::uint64_t Hash64Combine(std::uint64_t a, std::uint64_t b)
{
    return Hash64(a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
}

static std::uint64_t ComputeFontKey(const AnsiCanvas& canvas, int cell_w_px, int cell_h_px, int scale)
{
    const fonts::FontId id = canvas.GetFontId();
    const fonts::FontInfo& finfo = fonts::Get(id);
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap);

    std::uint64_t k = 0;
    k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)cell_w_px);
    k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)cell_h_px);
    k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)scale);

    if (bitmap_font)
    {
        k = Hash64Combine(k, 0xB17B17B17ull); // tag
        k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)id);
        if (embedded_font)
        {
            // Embedded fonts are per-canvas and may change with file loads.
            // Key by bitmap storage identity + parameters.
            const std::uint64_t p = (std::uint64_t)(uintptr_t)ef->bitmap.data();
            k = Hash64Combine(k, p);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)ef->bitmap.size());
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)ef->cell_w);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)ef->cell_h);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)ef->glyph_count);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)(ef->vga_9col_dup ? 1 : 0));
        }
        else
        {
            k = Hash64Combine(k, (std::uint64_t)(uintptr_t)finfo.bitmap);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)finfo.cell_w);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)finfo.cell_h);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)(finfo.vga_9col_dup ? 1 : 0));
        }
    }
    else
    {
        k = Hash64Combine(k, 0xA71A5A71Aull); // tag

        ImFont* font = ImGui::GetFont();
        ImFontAtlas* atlas = nullptr;
        if (font)
            atlas = font->OwnerAtlas ? font->OwnerAtlas : ImGui::GetIO().Fonts;

        k = Hash64Combine(k, (std::uint64_t)(uintptr_t)font);
        k = Hash64Combine(k, (std::uint64_t)(uintptr_t)atlas);
        if (atlas)
        {
            // ImGui forks differ in what fields exist on ImFontAtlas (TexWidth/TexHeight/TexID may not).
            // Use GetTexDataAsRGBA32 (stable API) to key off the backing atlas image instead.
            unsigned char* rgba = nullptr;
            int aw = 0;
            int ah = 0;
            atlas->GetTexDataAsRGBA32(&rgba, &aw, &ah);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)aw);
            k = Hash64Combine(k, (std::uint64_t)(std::uint32_t)ah);
            k = Hash64Combine(k, (std::uint64_t)(uintptr_t)rgba);
        }

        // Tie to current font size (affects glyph metrics/placement in our rasterizer).
        const float fs = ImGui::GetFontSize();
        const std::uint64_t fs_q = (std::uint64_t)(std::uint32_t)std::lround((double)fs * 1000.0);
        k = Hash64Combine(k, fs_q);
    }

    return k;
}
} // namespace

GlyphMaskCache::Mask GlyphMaskCache::GetMask(const AnsiCanvas& canvas,
                                            int cell_w_px,
                                            int cell_h_px,
                                            int scale,
                                            AnsiCanvas::GlyphId glyph,
                                            std::string& err)
{
    err.clear();
    cell_w_px = std::max(1, cell_w_px);
    cell_h_px = std::max(1, cell_h_px);
    scale = std::clamp(scale, 1, 16);

    Key key;
    key.font_key = ComputeFontKey(canvas, cell_w_px, cell_h_px, scale);
    key.cell_w_px = cell_w_px;
    key.cell_h_px = cell_h_px;
    key.scale = scale;
    key.glyph = (std::uint32_t)glyph;

    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;

    Mask m;
    m.w = cell_w_px;
    m.h = cell_h_px;
    m.a.assign((size_t)m.w * (size_t)m.h, 0u);

    const fonts::FontId id = canvas.GetFontId();
    const fonts::FontInfo& finfo = fonts::Get(id);
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font = embedded_font || (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap);

    if (!bitmap_font)
    {
        ImFont* font = ImGui::GetFont();
        if (!font)
        {
            err = "No active ImGui font.";
            cache_.emplace(key, m);
            return m;
        }

        ImFontAtlas* atlas = font->OwnerAtlas ? font->OwnerAtlas : ImGui::GetIO().Fonts;
        if (!atlas)
        {
            err = "No ImGui font atlas.";
            cache_.emplace(key, m);
            return m;
        }

        unsigned char* atlas_rgba = nullptr;
        int atlas_w = 0, atlas_h = 0;
        atlas->GetTexDataAsRGBA32(&atlas_rgba, &atlas_w, &atlas_h);
        if (!atlas_rgba || atlas_w <= 0 || atlas_h <= 0)
        {
            err = "ImGui font atlas has no RGBA texture data.";
            cache_.emplace(key, m);
            return m;
        }

        ImFontBaked* baked = ImGui::GetFontBaked();
        if (!baked)
        {
            const float bake_size = (font->LegacySize > 0.0f) ? font->LegacySize : 16.0f;
            baked = const_cast<ImFont*>(font)->GetFontBaked(bake_size);
        }

        const char32_t cp = phos::glyph::ToUnicodeRepresentative((phos::GlyphId)glyph);
        const ImFontGlyph* g = baked ? baked->FindGlyphNoFallback((ImWchar)cp) : nullptr;
        if (!g)
        {
            cache_.emplace(key, m);
            return m;
        }

        const int gx0 = std::clamp((int)std::floor(g->U0 * (float)atlas_w), 0, atlas_w);
        const int gy0 = std::clamp((int)std::floor(g->V0 * (float)atlas_h), 0, atlas_h);
        const int gx1 = std::clamp((int)std::ceil(g->U1 * (float)atlas_w), 0, atlas_w);
        const int gy1 = std::clamp((int)std::ceil(g->V1 * (float)atlas_h), 0, atlas_h);
        const int gw = std::max(0, gx1 - gx0);
        const int gh = std::max(0, gy1 - gy0);
        if (gw <= 0 || gh <= 0)
        {
            cache_.emplace(key, m);
            return m;
        }

        // Center glyph rect within the cell (must match canvas_rasterizer).
        const int off_x = (cell_w_px - gw) / 2;
        const int off_y = (cell_h_px - gh) / 2;

        for (int sy = 0; sy < gh; ++sy)
        {
            for (int sx = 0; sx < gw; ++sx)
            {
                const size_t abase = (size_t)((gy0 + sy) * atlas_w + (gx0 + sx)) * 4u;
                const std::uint8_t a8 = atlas_rgba[abase + 3];
                if (a8 == 0)
                    continue;

                const int dx = off_x + sx;
                const int dy = off_y + sy;
                if ((unsigned)dx >= (unsigned)cell_w_px || (unsigned)dy >= (unsigned)cell_h_px)
                    continue;
                const size_t di = (size_t)dy * (size_t)cell_w_px + (size_t)dx;
                m.a[di] = (std::uint8_t)std::max<int>(m.a[di], (int)a8);
            }
        }

        cache_.emplace(key, m);
        return m;
    }

    // Bitmap font path: generate mask using the same scaling rules as canvas_rasterizer.
    int glyph_cell_w = finfo.cell_w;
    int glyph_cell_h = finfo.cell_h;
    bool vga_dup = finfo.vga_9col_dup;

    if (embedded_font)
    {
        glyph_cell_w = ef->cell_w;
        glyph_cell_h = ef->cell_h;
        vga_dup = ef->vga_9col_dup;
    }
    const std::uint16_t glyph_index = phos::glyph::ResolveBitmapGlyph(finfo, ef, (phos::GlyphId)glyph).glyph_index;

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

    const std::uint8_t glyph8 = (std::uint8_t)(glyph_index & 0xFFu);

    // IMPORTANT: match canvas_rasterizer integer scaling behavior.
    const int px_w = std::max(1, (cell_w_px * scale) / std::max(1, glyph_cell_w));
    const int px_h = std::max(1, (cell_h_px * scale) / std::max(1, glyph_cell_h));

    auto bit_set = [&](std::uint8_t bits, int x) -> bool
    {
        if (x < 8)
            return (bits & (std::uint8_t)(0x80u >> x)) != 0;
        if (x == 8 && vga_dup && glyph_cell_w == 9 && glyph8 >= 192 && glyph8 <= 223)
            return (bits & 0x01u) != 0;
        return false;
    };

    for (int yy = 0; yy < glyph_cell_h; ++yy)
    {
        const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
        for (int xx = 0; xx < glyph_cell_w; ++xx)
        {
            if (!bit_set(bits, xx))
                continue;

            const int dx0 = xx * px_w;
            const int dy0 = yy * px_h;
            const int dx1 = dx0 + px_w;
            const int dy1 = dy0 + px_h;
            for (int y = dy0; y < dy1; ++y)
            {
                if ((unsigned)y >= (unsigned)cell_h_px)
                    continue;
                for (int x = dx0; x < dx1; ++x)
                {
                    if ((unsigned)x >= (unsigned)cell_w_px)
                        continue;
                    m.a[(size_t)y * (size_t)cell_w_px + (size_t)x] = 255u;
                }
            }
        }
    }

    cache_.emplace(key, m);
    return m;
}
} // namespace deform


