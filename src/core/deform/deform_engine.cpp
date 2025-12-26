#include "core/deform/deform_engine.h"

#include "core/canvas_rasterizer.h"
#include "core/colour_system.h"
#include "core/deform/glyph_mask_cache.h"
#include "core/xterm256_palette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace deform
{
namespace
{
static inline AnsiCanvas::Rect IntersectRects(const AnsiCanvas::Rect& a, const AnsiCanvas::Rect& b)
{
    const int ax0 = a.x;
    const int ay0 = a.y;
    const int ax1 = a.x + a.w;
    const int ay1 = a.y + a.h;

    const int bx0 = b.x;
    const int by0 = b.y;
    const int bx1 = b.x + b.w;
    const int by1 = b.y + b.h;

    const int x0 = std::max(ax0, bx0);
    const int y0 = std::max(ay0, by0);
    const int x1 = std::min(ax1, bx1);
    const int y1 = std::min(ay1, by1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0)
        return {};
    return AnsiCanvas::Rect{x0, y0, w, h};
}

static inline AnsiCanvas::Rect ClampToCanvas(const AnsiCanvas& canvas, const AnsiCanvas::Rect& r)
{
    const int cols = canvas.GetColumns();
    const int rows = canvas.GetRows();
    if (cols <= 0 || rows <= 0)
        return {};
    const AnsiCanvas::Rect bounds{0, 0, cols, rows};
    return IntersectRects(r, bounds);
}

static inline AnsiCanvas::Rect DabBoundsCell(float cx, float cy, int size_cells)
{
    const int d = std::max(1, size_cells);
    const float r = (float)d * 0.5f;
    const int x0 = (int)std::floor(cx - r);
    const int y0 = (int)std::floor(cy - r);
    const int x1 = (int)std::ceil(cx + r);
    const int y1 = (int)std::ceil(cy + r);
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0)
        return {};
    return AnsiCanvas::Rect{x0, y0, w, h};
}

static inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

static inline float Smoothstep(float t)
{
    t = Clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

static inline float FalloffFromDistance(float d01, float hardness01)
{
    // d01 is ellipse distance in [0..1] where 1 is boundary.
    if (d01 >= 1.0f)
        return 0.0f;
    if (hardness01 >= 1.0f)
        return 1.0f;

    const float r = std::sqrt(std::max(0.0f, d01));
    const float inner = std::clamp(hardness01, 0.0f, 1.0f);
    if (r <= inner)
        return 1.0f;
    const float t = (r - inner) / std::max(1e-6f, (1.0f - inner));
    return 1.0f - Smoothstep(t);
}

struct RgbaF
{
    float r = 0, g = 0, b = 0, a = 0;
};

static inline RgbaF BilinearSampleClamp(const std::vector<std::uint8_t>& src, int w, int h, float x, float y)
{
    if (w <= 0 || h <= 0 || src.size() < (size_t)w * (size_t)h * 4u)
        return {};

    x = std::clamp(x, 0.0f, (float)std::max(0, w - 1));
    y = std::clamp(y, 0.0f, (float)std::max(0, h - 1));

    const int x0 = (int)std::floor(x);
    const int y0 = (int)std::floor(y);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;

    auto load = [&](int ix, int iy) -> RgbaF {
        const size_t i = ((size_t)iy * (size_t)w + (size_t)ix) * 4u;
        return {
            (float)src[i + 0] / 255.0f,
            (float)src[i + 1] / 255.0f,
            (float)src[i + 2] / 255.0f,
            (float)src[i + 3] / 255.0f,
        };
    };

    const RgbaF c00 = load(x0, y0);
    const RgbaF c10 = load(x1, y0);
    const RgbaF c01 = load(x0, y1);
    const RgbaF c11 = load(x1, y1);

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    RgbaF cx0{lerp(c00.r, c10.r, fx), lerp(c00.g, c10.g, fx), lerp(c00.b, c10.b, fx), lerp(c00.a, c10.a, fx)};
    RgbaF cx1{lerp(c01.r, c11.r, fx), lerp(c01.g, c11.g, fx), lerp(c01.b, c11.b, fx), lerp(c01.a, c11.a, fx)};
    return {lerp(cx0.r, cx1.r, fy), lerp(cx0.g, cx1.g, fy), lerp(cx0.b, cx1.b, fy), lerp(cx0.a, cx1.a, fy)};
}

static inline void StoreRgba(const RgbaF& c, std::uint8_t* dst4)
{
    auto to_u8 = [](float v) -> std::uint8_t {
        const int i = (int)std::lround((double)std::clamp(v, 0.0f, 1.0f) * 255.0);
        return (std::uint8_t)std::clamp(i, 0, 255);
    };
    dst4[0] = to_u8(c.r);
    dst4[1] = to_u8(c.g);
    dst4[2] = to_u8(c.b);
    dst4[3] = to_u8(c.a);
}

static inline int SnapToAllowedPaletteIndex(phos::colour::PaletteRegistry& reg,
                                            phos::colour::LutCache& luts,
                                            phos::colour::PaletteInstanceId pal,
                                            std::uint8_t r,
                                            std::uint8_t g,
                                            std::uint8_t b,
                                            const std::vector<int>* allowed)
{
    const phos::colour::QuantizePolicy qpol = phos::colour::DefaultQuantizePolicy();
    if (!allowed || allowed->empty())
        return (int)phos::colour::ColourOps::NearestIndexRgb(reg, pal, r, g, b, qpol);

    // LUT-backed allowed quantization (coarse RGB 3D LUT).
    const auto qlut = luts.GetOrBuildAllowedQuant3d(reg, pal, *allowed, /*bits=*/5, qpol);
    if (qlut && qlut->bits > 0)
    {
        const int shift = 8 - (int)qlut->bits;
        const std::size_t side = (std::size_t)1u << qlut->bits;
        const std::size_t rx = (std::size_t)(r >> shift);
        const std::size_t gy = (std::size_t)(g >> shift);
        const std::size_t bz = (std::size_t)(b >> shift);
        const std::size_t flat = (bz * side + gy) * side + rx;
        return (int)qlut->table[flat];
    }

    // Fallback: exact scan (previous behavior) if LUT can't be built (budget pressure, etc).
    const phos::colour::Palette* p = reg.Get(pal);
    if (!p || p->rgb.empty())
        return (int)phos::colour::ColourOps::NearestIndexRgb(reg, pal, r, g, b, qpol);
    int best = -1;
    int best_d = 0;
    for (int idx : *allowed)
    {
        if (idx < 0 || idx >= (int)p->rgb.size())
            continue;
        const phos::colour::Rgb8 prgb = p->rgb[(size_t)idx];
        const int dr = (int)prgb.r - (int)r;
        const int dg = (int)prgb.g - (int)g;
        const int db = (int)prgb.b - (int)b;
        const int d = dr * dr + dg * dg + db * db;
        // Determinism: if distances tie, choose the lowest palette index.
        if (best < 0 || d < best_d || (d == best_d && idx < best))
        {
            best = idx;
            best_d = d;
        }
    }
    if (best < 0)
        best = (int)phos::colour::ColourOps::NearestIndexRgb(reg, pal, r, g, b, qpol);
    return best;
}

static inline void AddAsciiCandidates(std::vector<char32_t>& out)
{
    for (char32_t cp = 32; cp <= 126; ++cp)
        out.push_back(cp);
}

static inline void AddBasicBlockCandidates(std::vector<char32_t>& out)
{
    // Minimal set that tends to be stable for ANSI art.
    out.push_back(U' ');
    out.push_back(U'\u2588'); // full block
    out.push_back(U'\u2593'); // dark shade
    out.push_back(U'\u2592'); // medium shade
    out.push_back(U'\u2591'); // light shade
    out.push_back(U'\u2580'); // upper half block
    out.push_back(U'\u2584'); // lower half block
    out.push_back(U'\u258C'); // left half block
    out.push_back(U'\u2590'); // right half block
}

static inline void SnapshotLayerRegion(const AnsiCanvas& canvas,
                                      int layer_index,
                                      const AnsiCanvas::Rect& r,
                                      std::vector<AnsiCanvas::GlyphId>& glyph,
                                      std::vector<AnsiCanvas::ColourIndex16>& fg,
                                      std::vector<AnsiCanvas::ColourIndex16>& bg,
                                      std::vector<AnsiCanvas::Attrs>& attrs)
{
    const size_t n = (size_t)std::max(0, r.w) * (size_t)std::max(0, r.h);
    glyph.resize(n, phos::glyph::MakeUnicodeScalar(U' '));
    fg.resize(n, AnsiCanvas::kUnsetIndex16);
    bg.resize(n, AnsiCanvas::kUnsetIndex16);
    attrs.resize(n, 0);
    if (r.w <= 0 || r.h <= 0)
        return;

    for (int row = r.y; row < r.y + r.h; ++row)
    {
        for (int col = r.x; col < r.x + r.w; ++col)
        {
            const size_t i = (size_t)(row - r.y) * (size_t)r.w + (size_t)(col - r.x);
            glyph[i] = canvas.GetLayerGlyph(layer_index, row, col);
            (void)canvas.GetLayerCellIndices(layer_index, row, col, fg[i], bg[i]);
            (void)canvas.GetLayerCellAttrs(layer_index, row, col, attrs[i]);
        }
    }
}

struct InverseMapResultCell
{
    float sx = 0.0f;
    float sy = 0.0f;
    float w = 0.0f;
    bool inside = false;
};

static inline InverseMapResultCell InverseMapCell(const ApplyDabArgs& args,
                                                  int size_cells,
                                                  float px,
                                                  float py)
{
    InverseMapResultCell r;
    const float radius_cells = (float)std::max(1, size_cells) * 0.5f;
    const float rx = std::max(1e-6f, radius_cells);
    const float ry = std::max(1e-6f, radius_cells);

    const float hardness = Clamp01(args.hardness);
    const float strength = Clamp01(args.strength);
    const float amount = std::max(0.0f, args.amount);

    const float dx = px - args.x;
    const float dy = py - args.y;
    const float d01 = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
    if (d01 >= 1.0f)
        return r;

    const float w = FalloffFromDistance(d01, hardness) * strength;
    if (w <= 0.0f)
        return r;

    float sx = px;
    float sy = py;

    constexpr float kTwoPi = 6.2831853071795864769f;
    const float theta_max = kTwoPi * amount;

    switch (args.mode)
    {
        case Mode::Move:
        {
            if (!args.prev_x.has_value() || !args.prev_y.has_value())
                return r;
            const float dx_move = (args.x - *args.prev_x);
            const float dy_move = (args.y - *args.prev_y);
            sx = px - dx_move * w;
            sy = py - dy_move * w;
        } break;
        case Mode::Grow:
        case Mode::Shrink:
        {
            const float sign = (args.mode == Mode::Grow) ? 1.0f : -1.0f;
            float s = 1.0f + sign * (w * amount);
            s = std::clamp(s, 0.25f, 4.0f);
            sx = args.x + dx / s;
            sy = args.y + dy / s;
        } break;
        case Mode::SwirlCw:
        case Mode::SwirlCcw:
        {
            const float sign = (args.mode == Mode::SwirlCw) ? 1.0f : -1.0f;
            const float theta = -sign * theta_max * w; // inverse rotation
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            const float rdx = c * dx - s * dy;
            const float rdy = s * dx + c * dy;
            sx = args.x + rdx;
            sy = args.y + rdy;
        } break;
    }

    r.sx = sx;
    r.sy = sy;
    r.w = w;
    r.inside = true;
    return r;
}
} // namespace

ApplyDabResult DeformEngine::ApplyDab(AnsiCanvas& canvas,
                                     int layer_index,
                                     const ApplyDabArgs& args,
                                     std::string& err) const
{
    err.clear();

    auto& cs = phos::colour::GetColourSystem();
    phos::colour::PaletteInstanceId pal = cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
    if (auto id = cs.Palettes().Resolve(args.palette_ref))
        pal = *id;
    const phos::colour::Palette* pal_def = cs.Palettes().Get(pal);
    const int pal_max_idx = (pal_def && !pal_def->rgb.empty()) ? (int)pal_def->rgb.size() - 1 : 255;

    if (layer_index < 0 || layer_index >= canvas.GetLayerCount())
    {
        err = "Invalid layer index.";
        return {};
    }

    const int size_cells = std::max(1, args.size);
    const AnsiCanvas::Rect dab = DabBoundsCell(args.x, args.y, size_cells);
    if (dab.w <= 0 || dab.h <= 0)
        return {};

    // Default clip = full canvas bounds.
    AnsiCanvas::Rect clip = args.clip;
    if (clip.w <= 0 || clip.h <= 0)
        clip = AnsiCanvas::Rect{0, 0, canvas.GetColumns(), canvas.GetRows()};

    const AnsiCanvas::Rect clipped = IntersectRects(ClampToCanvas(canvas, dab), ClampToCanvas(canvas, clip));
    if (clipped.w <= 0 || clipped.h <= 0)
        return {};

    // ---------------------------------------------------------------------
    // Cell-resample algorithm: inverse-map per-cell and copy from a source snapshot.
    // This avoids introducing new glyphs during deformation by design.
    // ---------------------------------------------------------------------
    const bool move_mode = (args.mode == Mode::Move);
    if (args.algo == DeformAlgo::CellResample)
    {
        if (move_mode && (!args.prev_x.has_value() || !args.prev_y.has_value()))
        {
            // Krita behavior: first move dab is a no-op (needs a previous point).
            ApplyDabResult res;
            res.changed = false;
            res.affected = clipped;
            return res;
        }

        std::vector<AnsiCanvas::GlyphId> src_glyph;
        std::vector<AnsiCanvas::ColourIndex16> src_fg;
        std::vector<AnsiCanvas::ColourIndex16> src_bg;
        std::vector<AnsiCanvas::Attrs> src_attrs;
        SnapshotLayerRegion(canvas, layer_index, clipped, src_glyph, src_fg, src_bg, src_attrs);

        ApplyDabResult res;
        res.changed = false;
        res.affected = clipped;

        for (int row = clipped.y; row < clipped.y + clipped.h; ++row)
        {
            for (int col = clipped.x; col < clipped.x + clipped.w; ++col)
            {
                // Cell center in cell coordinates.
                const float px = (float)col + 0.5f;
                const float py = (float)row + 0.5f;
                const InverseMapResultCell im = InverseMapCell(args, size_cells, px, py);
                if (!im.inside || im.w <= 0.0f)
                    continue;

                // Inverse-map: clamp to region bounds to match raster path's clamped sampling.
                int src_col = (int)std::floor(im.sx);
                int src_row = (int)std::floor(im.sy);
                src_col = std::clamp(src_col, clipped.x, clipped.x + clipped.w - 1);
                src_row = std::clamp(src_row, clipped.y, clipped.y + clipped.h - 1);

                const size_t si = (size_t)(src_row - clipped.y) * (size_t)clipped.w + (size_t)(src_col - clipped.x);
                const AnsiCanvas::GlyphId new_glyph =
                    (si < src_glyph.size()) ? src_glyph[si] : (AnsiCanvas::GlyphId)phos::glyph::MakeUnicodeScalar(U' ');
                const AnsiCanvas::ColourIndex16 new_fg =
                    (si < src_fg.size()) ? src_fg[si] : AnsiCanvas::kUnsetIndex16;
                const AnsiCanvas::ColourIndex16 new_bg =
                    (si < src_bg.size()) ? src_bg[si] : AnsiCanvas::kUnsetIndex16;
                const AnsiCanvas::Attrs new_attrs = (si < src_attrs.size()) ? src_attrs[si] : 0;

                // Apply if changed.
                const AnsiCanvas::GlyphId old_glyph = canvas.GetLayerGlyph(layer_index, row, col);
                AnsiCanvas::ColourIndex16 old_fg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::ColourIndex16 old_bg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::Attrs old_attrs = 0;
                (void)canvas.GetLayerCellIndices(layer_index, row, col, old_fg, old_bg);
                (void)canvas.GetLayerCellAttrs(layer_index, row, col, old_attrs);

                if (old_glyph != new_glyph || old_fg != new_fg || old_bg != new_bg || old_attrs != new_attrs)
                {
                    (void)canvas.SetLayerGlyphIndicesPartial(layer_index,
                                                            row,
                                                            col,
                                                            new_glyph,
                                                            new_fg,
                                                            new_bg,
                                                            new_attrs);
                    res.changed = true;
                }
            }
        }

        return res;
    }

    // ---------------------------------------------------------------------
    // Candidate glyph sets:
    // - base_candidates: explicit list / ASCII / blocks (host-provided palette, etc)
    // - region_candidates: glyphs already present in the affected region
    // ---------------------------------------------------------------------
    std::vector<AnsiCanvas::GlyphId> base_candidates;
    base_candidates.reserve(512);
    switch (args.glyph_set.kind)
    {
        case GlyphSetKind::ExplicitList:
            if (!args.glyph_set.explicit_glyph_ids.empty())
            {
                for (AnsiCanvas::GlyphId g : args.glyph_set.explicit_glyph_ids)
                    if (g != 0)
                        base_candidates.push_back(g);
            }
            else
            {
                for (char32_t cp : args.glyph_set.explicit_codepoints)
                    if (cp != 0)
                        base_candidates.push_back(phos::glyph::MakeUnicodeScalar(cp));
            }
            break;
        case GlyphSetKind::Ascii:
            {
                std::vector<char32_t> tmp;
                AddAsciiCandidates(tmp);
                for (char32_t cp : tmp)
                    base_candidates.push_back(phos::glyph::MakeUnicodeScalar(cp));
            }
            break;
        case GlyphSetKind::BasicBlocks:
            {
                std::vector<char32_t> tmp;
                AddBasicBlockCandidates(tmp);
                for (char32_t cp : tmp)
                    base_candidates.push_back(phos::glyph::MakeUnicodeScalar(cp));
            }
            break;
        case GlyphSetKind::FontAll:
            // Not supported in v1 (too expensive for atlas fonts). Fall back to blocks.
            {
                std::vector<char32_t> tmp;
                AddBasicBlockCandidates(tmp);
                for (char32_t cp : tmp)
                    base_candidates.push_back(phos::glyph::MakeUnicodeScalar(cp));
            }
            break;
    }

    // Add glyphs already present on the canvas in the affected region.
    std::vector<AnsiCanvas::GlyphId> region_candidates;
    {
        std::unordered_set<AnsiCanvas::GlyphId> seen;
        seen.reserve((size_t)clipped.w * (size_t)clipped.h);
        // Preserve order: seed with existing base candidates first.
        for (AnsiCanvas::GlyphId g : base_candidates)
            if (g != 0)
                seen.insert(g);

        for (int row = clipped.y; row < clipped.y + clipped.h; ++row)
        {
            for (int col = clipped.x; col < clipped.x + clipped.w; ++col)
            {
                AnsiCanvas::GlyphId glyph = phos::glyph::MakeUnicodeScalar(U' ');
                AnsiCanvas::ColourIndex16 fg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::ColourIndex16 bg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::Attrs a = 0;
                if (args.sample == Sample::Composite)
                    (void)canvas.GetCompositeCellPublicGlyphIndices(row, col, glyph, fg, bg, a);
                else
                {
                    glyph = canvas.GetLayerGlyph(layer_index, row, col);
                    (void)canvas.GetLayerCellIndices(layer_index, row, col, fg, bg);
                    (void)canvas.GetLayerCellAttrs(layer_index, row, col, a);
                }
                if (glyph == 0)
                    continue;
                if (seen.insert(glyph).second)
                {
                    if (region_candidates.size() < 512)
                        region_candidates.push_back(glyph);
                }
            }
        }
    }

    // Union candidates (bounded).
    std::vector<AnsiCanvas::GlyphId> candidates = base_candidates;
    if (candidates.size() < 512)
    {
        for (AnsiCanvas::GlyphId g : region_candidates)
        {
            if (g == 0)
                continue;
            if (std::find(candidates.begin(), candidates.end(), g) == candidates.end())
            {
                candidates.push_back(g);
                if (candidates.size() >= 512)
                    break;
            }
        }
    }

    // Always ensure space exists.
    if (std::find(candidates.begin(), candidates.end(), U' ') == candidates.end())
        candidates.insert(candidates.begin(), U' ');

    // ---------------------------------------------------------------------
    // Rasterize region to RGBA (with transparent unset backgrounds)
    // ---------------------------------------------------------------------
    canvas_rasterizer::Options ropt;
    ropt.scale = 1;
    ropt.transparent_unset_bg = true;

    std::vector<std::uint8_t> src_rgba;
    int src_w = 0;
    int src_h = 0;
    if (args.sample == Sample::Composite)
    {
        if (!canvas_rasterizer::RasterizeCompositeRegionToRgba32(canvas, clipped, src_rgba, src_w, src_h, err, ropt))
            return {};
    }
    else
    {
        if (!canvas_rasterizer::RasterizeLayerRegionToRgba32(canvas, layer_index, clipped, src_rgba, src_w, src_h, err, ropt))
            return {};
    }

    if (src_w <= 0 || src_h <= 0 || src_rgba.size() < (size_t)src_w * (size_t)src_h * 4u)
    {
        err = "Rasterization produced an empty buffer.";
        return {};
    }

    const int cell_w_px = std::max(1, src_w / std::max(1, clipped.w));
    const int cell_h_px = std::max(1, src_h / std::max(1, clipped.h));

    // ---------------------------------------------------------------------
    // Warp kernel (inverse-map + bilinear sample) in pixel space
    // ---------------------------------------------------------------------
    std::vector<std::uint8_t> dst_rgba = src_rgba; // start as identity copy

    const float cx_px = (args.x - (float)clipped.x) * (float)cell_w_px;
    const float cy_px = (args.y - (float)clipped.y) * (float)cell_h_px;
    const float radius_cells = (float)size_cells * 0.5f;
    const float rx = std::max(1.0f, radius_cells * (float)cell_w_px);
    const float ry = std::max(1.0f, radius_cells * (float)cell_h_px);

    const float hardness = Clamp01(args.hardness);
    const float strength = Clamp01(args.strength);
    const float amount = std::max(0.0f, args.amount);

    if (move_mode && (!args.prev_x.has_value() || !args.prev_y.has_value()))
    {
        // Krita behavior: first move dab is a no-op (needs a previous point).
        ApplyDabResult res;
        res.changed = false;
        res.affected = clipped;
        return res;
    }

    const float dx_move_px = move_mode ? ((args.x - *args.prev_x) * (float)cell_w_px) : 0.0f;
    const float dy_move_px = move_mode ? ((args.y - *args.prev_y) * (float)cell_h_px) : 0.0f;

    constexpr float kTwoPi = 6.2831853071795864769f;
    const float theta_max = kTwoPi * amount;

    for (int y = 0; y < src_h; ++y)
    {
        for (int x = 0; x < src_w; ++x)
        {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            const float dx = px - cx_px;
            const float dy = py - cy_px;
            const float d01 = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
            if (d01 >= 1.0f)
                continue;
            const float w = FalloffFromDistance(d01, hardness) * strength;
            if (w <= 0.0f)
                continue;

            float sx = px;
            float sy = py;

            switch (args.mode)
            {
                case Mode::Move:
                {
                    sx = px - dx_move_px * w;
                    sy = py - dy_move_px * w;
                } break;
                case Mode::Grow:
                case Mode::Shrink:
                {
                    const float sign = (args.mode == Mode::Grow) ? 1.0f : -1.0f;
                    float s = 1.0f + sign * (w * amount);
                    s = std::clamp(s, 0.25f, 4.0f);
                    sx = cx_px + dx / s;
                    sy = cy_px + dy / s;
                } break;
                case Mode::SwirlCw:
                case Mode::SwirlCcw:
                {
                    const float sign = (args.mode == Mode::SwirlCw) ? 1.0f : -1.0f;
                    const float theta = -sign * theta_max * w; // inverse rotation
                    const float c = std::cos(theta);
                    const float s = std::sin(theta);
                    const float rdx = c * dx - s * dy;
                    const float rdy = s * dx + c * dy;
                    sx = cx_px + rdx;
                    sy = cy_px + rdy;
                } break;
            }

            // Convert back to pixel indices (sample expects pixel centers at +0.5).
            const float sample_x = sx - 0.5f;
            const float sample_y = sy - 0.5f;
            const RgbaF samp = BilinearSampleClamp(src_rgba, src_w, src_h, sample_x, sample_y);
            std::uint8_t* outp = &dst_rgba[((size_t)y * (size_t)src_w + (size_t)x) * 4u];
            StoreRgba(samp, outp);
        }
    }

    // ---------------------------------------------------------------------
    // Quantize back to cells (attrs forced to 0; bg==0 supported early)
    // ---------------------------------------------------------------------
    thread_local GlyphMaskCache mask_cache;
    std::string mask_err;

    // For sticky warp+quantize, we want a stable "source glyph" anchor from the edited layer.
    std::vector<AnsiCanvas::GlyphId> src_glyph_for_anchor;
    std::vector<AnsiCanvas::ColourIndex16> tmp_fg;
    std::vector<AnsiCanvas::ColourIndex16> tmp_bg;
    std::vector<AnsiCanvas::Attrs> tmp_attrs;
    if (args.algo == DeformAlgo::WarpQuantizeSticky)
        SnapshotLayerRegion(canvas, layer_index, clipped, src_glyph_for_anchor, tmp_fg, tmp_bg, tmp_attrs);

    auto compute_error = [&](const std::vector<std::uint8_t>& target_mask, const GlyphMaskCache::Mask& gm) -> double {
        const size_t n = (size_t)cell_w_px * (size_t)cell_h_px;
        if (gm.a.size() < n || target_mask.size() < n)
            return 1e30;
        double e = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const int t = (int)target_mask[i];
            const int m = (int)gm.a[i];
            const int d = t - m;
            e += (double)d * (double)d;
        }
        return e;
    };

    ApplyDabResult res;
    res.changed = false;
    res.affected = clipped;

    std::vector<std::uint8_t> target_mask;
    target_mask.resize((size_t)cell_w_px * (size_t)cell_h_px);

    for (int row = clipped.y; row < clipped.y + clipped.h; ++row)
    {
        for (int col = clipped.x; col < clipped.x + clipped.w; ++col)
        {
            const int local_col = col - clipped.x;
            const int local_row = row - clipped.y;
            const int bx0 = local_col * cell_w_px;
            const int by0 = local_row * cell_h_px;

            // Gather alpha stats and (optionally) colours.
            double sum_a = 0.0;
            int max_a = 0;
            int min_a = 255;
            double sum_wr = 0.0, sum_wg = 0.0, sum_wb = 0.0;
            double sum_w = 0.0;

            for (int yy = 0; yy < cell_h_px; ++yy)
            {
                for (int xx = 0; xx < cell_w_px; ++xx)
                {
                    const int px = bx0 + xx;
                    const int py = by0 + yy;
                    const size_t i = ((size_t)py * (size_t)src_w + (size_t)px) * 4u;
                    const int a8 = (int)dst_rgba[i + 3];
                    max_a = std::max(max_a, a8);
                    min_a = std::min(min_a, a8);
                    sum_a += (double)a8;

                    const double w = (double)a8 / 255.0;
                    if (w > 0.0)
                    {
                        sum_wr += (double)dst_rgba[i + 0] * w;
                        sum_wg += (double)dst_rgba[i + 1] * w;
                        sum_wb += (double)dst_rgba[i + 2] * w;
                        sum_w += w;
                    }
                }
            }

            const double avg_a = sum_a / (double)(cell_w_px * cell_h_px);
            if (max_a < 8)
            {
                // Fully transparent => unset bg + space.
                const char32_t old_cp = canvas.GetLayerCell(layer_index, row, col);
                AnsiCanvas::ColourIndex16 old_fg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::ColourIndex16 old_bg = AnsiCanvas::kUnsetIndex16;
                AnsiCanvas::Attrs old_attrs = 0;
                (void)canvas.GetLayerCellIndices(layer_index, row, col, old_fg, old_bg);
                (void)canvas.GetLayerCellAttrs(layer_index, row, col, old_attrs);

                const char32_t new_cp = U' ';
                const AnsiCanvas::ColourIndex16 new_fg = AnsiCanvas::kUnsetIndex16;
                const AnsiCanvas::ColourIndex16 new_bg = AnsiCanvas::kUnsetIndex16;
                const AnsiCanvas::Attrs new_attrs = 0;
                if (old_cp != new_cp || old_fg != new_fg || old_bg != new_bg || old_attrs != new_attrs)
                {
                    (void)canvas.SetLayerCellIndices(layer_index, row, col, new_cp, new_fg, new_bg, new_attrs);
                    res.changed = true;
                }
                continue;
            }

            // Decide "transparent bg" vs "opaque bg" mode.
            const bool prefer_unset_bg = (avg_a < 200.0); // mostly transparent => treat as bg=0

            // Build target mask.
            if (prefer_unset_bg)
            {
                // Target coverage = alpha.
                for (int yy = 0; yy < cell_h_px; ++yy)
                {
                    for (int xx = 0; xx < cell_w_px; ++xx)
                    {
                        const int px = bx0 + xx;
                        const int py = by0 + yy;
                        const size_t i = ((size_t)py * (size_t)src_w + (size_t)px) * 4u;
                        target_mask[(size_t)yy * (size_t)cell_w_px + (size_t)xx] = dst_rgba[i + 3];
                    }
                }
            }
            else
            {
                // Opaque-ish: derive a binary mask by comparing to a bg/fg guess via 2-means.
                // We keep it simple but bounded (no more than a few iterations).
                // Initialize centers from two samples.
                int c0r = 0, c0g = 0, c0b = 0;
                int c1r = 255, c1g = 255, c1b = 255;

                // Pick two seeds: min/max luminance.
                int best_lo = 1e9, best_hi = -1e9;
                for (int yy = 0; yy < cell_h_px; ++yy)
                {
                    for (int xx = 0; xx < cell_w_px; ++xx)
                    {
                        const size_t i = ((size_t)(by0 + yy) * (size_t)src_w + (size_t)(bx0 + xx)) * 4u;
                        const int r8 = (int)dst_rgba[i + 0];
                        const int g8 = (int)dst_rgba[i + 1];
                        const int b8 = (int)dst_rgba[i + 2];
                        const int lum = r8 * 30 + g8 * 59 + b8 * 11;
                        if (lum < best_lo)
                        {
                            best_lo = lum;
                            c0r = r8; c0g = g8; c0b = b8;
                        }
                        if (lum > best_hi)
                        {
                            best_hi = lum;
                            c1r = r8; c1g = g8; c1b = b8;
                        }
                    }
                }

                for (int iter = 0; iter < 3; ++iter)
                {
                    double s0r = 0, s0g = 0, s0b = 0, n0 = 0;
                    double s1r = 0, s1g = 0, s1b = 0, n1 = 0;
                    for (int yy = 0; yy < cell_h_px; ++yy)
                    {
                        for (int xx = 0; xx < cell_w_px; ++xx)
                        {
                            const size_t i = ((size_t)(by0 + yy) * (size_t)src_w + (size_t)(bx0 + xx)) * 4u;
                            const int r8 = (int)dst_rgba[i + 0];
                            const int g8 = (int)dst_rgba[i + 1];
                            const int b8 = (int)dst_rgba[i + 2];
                            const int d0 = (r8 - c0r) * (r8 - c0r) + (g8 - c0g) * (g8 - c0g) + (b8 - c0b) * (b8 - c0b);
                            const int d1 = (r8 - c1r) * (r8 - c1r) + (g8 - c1g) * (g8 - c1g) + (b8 - c1b) * (b8 - c1b);
                            if (d0 <= d1) { s0r += r8; s0g += g8; s0b += b8; n0 += 1; }
                            else { s1r += r8; s1g += g8; s1b += b8; n1 += 1; }
                        }
                    }
                    if (n0 > 0) { c0r = (int)std::lround(s0r / n0); c0g = (int)std::lround(s0g / n0); c0b = (int)std::lround(s0b / n0); }
                    if (n1 > 0) { c1r = (int)std::lround(s1r / n1); c1g = (int)std::lround(s1g / n1); c1b = (int)std::lround(s1b / n1); }
                }

                // Snap centers to palette.
                const int idx0 = SnapToAllowedPaletteIndex(cs.Palettes(), cs.Luts(), pal,
                                                          (std::uint8_t)std::clamp(c0r, 0, 255),
                                                          (std::uint8_t)std::clamp(c0g, 0, 255),
                                                          (std::uint8_t)std::clamp(c0b, 0, 255),
                                                          args.allowed_indices);
                const int idx1 = SnapToAllowedPaletteIndex(cs.Palettes(), cs.Luts(), pal,
                                                          (std::uint8_t)std::clamp(c1r, 0, 255),
                                                          (std::uint8_t)std::clamp(c1g, 0, 255),
                                                          (std::uint8_t)std::clamp(c1b, 0, 255),
                                                          args.allowed_indices);
                int bgr = 0, bgg = 0, bgb = 0;
                int fgr = 255, fgg = 255, fgb = 255;
                if (pal_def && !pal_def->rgb.empty())
                {
                    const int bg_i = std::clamp(idx0, 0, (int)pal_def->rgb.size() - 1);
                    const int fg_i = std::clamp(idx1, 0, (int)pal_def->rgb.size() - 1);
                    const phos::colour::Rgb8 bg_rgb = pal_def->rgb[(size_t)bg_i];
                    const phos::colour::Rgb8 fg_rgb = pal_def->rgb[(size_t)fg_i];
                    bgr = (int)bg_rgb.r; bgg = (int)bg_rgb.g; bgb = (int)bg_rgb.b;
                    fgr = (int)fg_rgb.r; fgg = (int)fg_rgb.g; fgb = (int)fg_rgb.b;
                }
                else
                {
                    // Fallback: go through the packed-colour bridge (shouldn't happen for built-in palettes).
                    const std::uint32_t bg32 = phos::colour::ColourOps::IndexToColour32(cs.Palettes(), pal, phos::colour::ColourIndex{(std::uint16_t)idx0});
                    const std::uint32_t fg32 = phos::colour::ColourOps::IndexToColour32(cs.Palettes(), pal, phos::colour::ColourIndex{(std::uint16_t)idx1});
                    std::uint8_t bgr8 = 0, bgg8 = 0, bgb8 = 0;
                    std::uint8_t fgr8 = 0, fgg8 = 0, fgb8 = 0;
                    (void)phos::colour::ColourOps::UnpackImGuiAbgr(bg32, bgr8, bgg8, bgb8);
                    (void)phos::colour::ColourOps::UnpackImGuiAbgr(fg32, fgr8, fgg8, fgb8);
                    bgr = (int)bgr8; bgg = (int)bgg8; bgb = (int)bgb8;
                    fgr = (int)fgr8; fgg = (int)fgg8; fgb = (int)fgb8;
                }

                for (int yy = 0; yy < cell_h_px; ++yy)
                {
                    for (int xx = 0; xx < cell_w_px; ++xx)
                    {
                        const size_t i = ((size_t)(by0 + yy) * (size_t)src_w + (size_t)(bx0 + xx)) * 4u;
                        const int r8 = (int)dst_rgba[i + 0];
                        const int g8 = (int)dst_rgba[i + 1];
                        const int b8 = (int)dst_rgba[i + 2];
                        const int db = (r8 - bgr) * (r8 - bgr) + (g8 - bgg) * (g8 - bgg) + (b8 - bgb) * (b8 - bgb);
                        const int df = (r8 - fgr) * (r8 - fgr) + (g8 - fgg) * (g8 - fgg) + (b8 - fgb) * (b8 - fgb);
                        target_mask[(size_t)yy * (size_t)cell_w_px + (size_t)xx] = (df < db) ? 255u : 0u;
                    }
                }
            }

            // Choose glyph by mask correlation.
            AnsiCanvas::GlyphId best_glyph = phos::glyph::MakeUnicodeScalar(U' ');
            double best_err = 1e30;

            auto find_best = [&](const std::vector<AnsiCanvas::GlyphId>& pool, AnsiCanvas::GlyphId& out_glyph, double& out_err) {
                out_glyph = phos::glyph::MakeUnicodeScalar(U' ');
                out_err = 1e30;
                for (AnsiCanvas::GlyphId g : pool)
                {
                    GlyphMaskCache::Mask gm = mask_cache.GetMask(canvas, cell_w_px, cell_h_px, 1, g, mask_err);
                    const double e = compute_error(target_mask, gm);
                    if (e < out_err)
                    {
                        out_err = e;
                        out_glyph = g;
                    }
                }
            };

            if (args.algo == DeformAlgo::WarpQuantizeSticky && !region_candidates.empty() && !base_candidates.empty())
            {
                AnsiCanvas::GlyphId best_region_glyph = phos::glyph::MakeUnicodeScalar(U' ');
                double best_region_err = 1e30;
                find_best(region_candidates, best_region_glyph, best_region_err);

                AnsiCanvas::GlyphId best_base_glyph = phos::glyph::MakeUnicodeScalar(U' ');
                double best_base_err = 1e30;
                find_best(base_candidates, best_base_glyph, best_base_err);

                // Prefer region glyphs unless the base set is meaningfully better.
                constexpr double kImprove = 0.85; // base must be >=15% better to override region
                if (best_base_err < best_region_err * kImprove)
                {
                    best_glyph = best_base_glyph;
                    best_err = best_base_err;
                }
                else
                {
                    best_glyph = best_region_glyph;
                    best_err = best_region_err;
                }
            }
            else
            {
                find_best(candidates, best_glyph, best_err);
            }

            // Hysteresis: keep current glyph if it's close enough.
            const AnsiCanvas::GlyphId cur_glyph = canvas.GetLayerGlyph(layer_index, row, col);
            if (args.hysteresis > 0.0f && std::find(candidates.begin(), candidates.end(), cur_glyph) != candidates.end())
            {
                GlyphMaskCache::Mask gm_cur = mask_cache.GetMask(canvas, cell_w_px, cell_h_px, 1, cur_glyph, mask_err);
                const double e_cur = compute_error(target_mask, gm_cur);
                const double eps = (double)std::max(0.0f, args.hysteresis);
                if (e_cur <= best_err * (1.0 + eps))
                    best_glyph = cur_glyph;
            }

            // Sticky anchor: prefer the inverse-mapped *source* glyph if close enough.
            if (args.algo == DeformAlgo::WarpQuantizeSticky && args.hysteresis > 0.0f && !src_glyph_for_anchor.empty())
            {
                const float px_cell = (float)col + 0.5f;
                const float py_cell = (float)row + 0.5f;
                const InverseMapResultCell im = InverseMapCell(args, size_cells, px_cell, py_cell);
                if (im.inside && im.w > 0.0f)
                {
                    int src_col = (int)std::floor(im.sx);
                    int src_row = (int)std::floor(im.sy);
                    src_col = std::clamp(src_col, clipped.x, clipped.x + clipped.w - 1);
                    src_row = std::clamp(src_row, clipped.y, clipped.y + clipped.h - 1);
                    const size_t si = (size_t)(src_row - clipped.y) * (size_t)clipped.w + (size_t)(src_col - clipped.x);
                    const AnsiCanvas::GlyphId anchor_glyph =
                        (si < src_glyph_for_anchor.size()) ? src_glyph_for_anchor[si] : (AnsiCanvas::GlyphId)phos::glyph::MakeUnicodeScalar(U' ');
                    if (anchor_glyph != 0 && std::find(candidates.begin(), candidates.end(), anchor_glyph) != candidates.end())
                    {
                        GlyphMaskCache::Mask gm_anchor = mask_cache.GetMask(canvas, cell_w_px, cell_h_px, 1, anchor_glyph, mask_err);
                        const double e_anchor = compute_error(target_mask, gm_anchor);
                        const double eps_anchor = (double)std::clamp(args.hysteresis * 3.0f, 0.0f, 1.0f);
                        if (e_anchor <= best_err * (1.0 + eps_anchor))
                            best_glyph = anchor_glyph;
                    }
                }
            }

            // Pick colours.
            AnsiCanvas::ColourIndex16 out_fg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColourIndex16 out_bg = AnsiCanvas::kUnsetIndex16;

            if (prefer_unset_bg)
            {
                // bg remains unset; fg from alpha-weighted average.
                if (sum_w > 0.0)
                {
                    const int r8 = (int)std::lround(sum_wr / sum_w);
                    const int g8 = (int)std::lround(sum_wg / sum_w);
                    const int b8 = (int)std::lround(sum_wb / sum_w);
                    const int idx = SnapToAllowedPaletteIndex(cs.Palettes(), cs.Luts(), pal,
                                                             (std::uint8_t)std::clamp(r8, 0, 255),
                                                             (std::uint8_t)std::clamp(g8, 0, 255),
                                                             (std::uint8_t)std::clamp(b8, 0, 255),
                                                             args.allowed_indices);
                    out_fg = (AnsiCanvas::ColourIndex16)std::clamp(idx, 0, pal_max_idx);
                }
                out_bg = AnsiCanvas::kUnsetIndex16;
                if (phos::glyph::IsBlank((phos::GlyphId)best_glyph))
                    out_fg = AnsiCanvas::kUnsetIndex16;
            }
            else
            {
                // Opaque: estimate two colours from extremes (cheap) and snap.
                int lo_r = 255, lo_g = 255, lo_b = 255;
                int hi_r = 0, hi_g = 0, hi_b = 0;
                for (int yy = 0; yy < cell_h_px; ++yy)
                {
                    for (int xx = 0; xx < cell_w_px; ++xx)
                    {
                        const size_t i = ((size_t)(by0 + yy) * (size_t)src_w + (size_t)(bx0 + xx)) * 4u;
                        const int r8 = (int)dst_rgba[i + 0];
                        const int g8 = (int)dst_rgba[i + 1];
                        const int b8 = (int)dst_rgba[i + 2];
                        lo_r = std::min(lo_r, r8); lo_g = std::min(lo_g, g8); lo_b = std::min(lo_b, b8);
                        hi_r = std::max(hi_r, r8); hi_g = std::max(hi_g, g8); hi_b = std::max(hi_b, b8);
                    }
                }
                const int bg_idx = SnapToAllowedPaletteIndex(cs.Palettes(), cs.Luts(), pal,
                                                            (std::uint8_t)std::clamp(lo_r, 0, 255),
                                                            (std::uint8_t)std::clamp(lo_g, 0, 255),
                                                            (std::uint8_t)std::clamp(lo_b, 0, 255),
                                                            args.allowed_indices);
                const int fg_idx = SnapToAllowedPaletteIndex(cs.Palettes(), cs.Luts(), pal,
                                                            (std::uint8_t)std::clamp(hi_r, 0, 255),
                                                            (std::uint8_t)std::clamp(hi_g, 0, 255),
                                                            (std::uint8_t)std::clamp(hi_b, 0, 255),
                                                            args.allowed_indices);
                out_bg = (AnsiCanvas::ColourIndex16)std::clamp(bg_idx, 0, pal_max_idx);
                out_fg = (AnsiCanvas::ColourIndex16)std::clamp(fg_idx, 0, pal_max_idx);
                if (phos::glyph::IsBlank((phos::GlyphId)best_glyph))
                    out_fg = AnsiCanvas::kUnsetIndex16;
            }

            // Force attrs=0 (stable).
            const AnsiCanvas::Attrs out_attrs = 0;

            // Apply if changed.
            AnsiCanvas::GlyphId old_glyph = canvas.GetLayerGlyph(layer_index, row, col);
            AnsiCanvas::ColourIndex16 old_fg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColourIndex16 old_bg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::Attrs old_attrs = 0;
            (void)canvas.GetLayerCellIndices(layer_index, row, col, old_fg, old_bg);
            (void)canvas.GetLayerCellAttrs(layer_index, row, col, old_attrs);

            if (old_glyph != best_glyph || old_fg != out_fg || old_bg != out_bg || old_attrs != out_attrs)
            {
                (void)canvas.SetLayerGlyphIndicesPartial(layer_index,
                                                        row,
                                                        col,
                                                        best_glyph,
                                                        out_fg,
                                                        out_bg,
                                                        out_attrs);
                res.changed = true;
            }
        }
    }

    return res;
}
} // namespace deform



