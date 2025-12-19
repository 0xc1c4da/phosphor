// Chafa RGBA -> ANSI conversion implementation.

#include "io/convert/chafa_convert.h"

#include "io/formats/ansi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Chafa (C library)
#include <chafa.h>

namespace chafa_convert
{
namespace
{
static void DebugPrintEscapedAnsiBytes(const char* label, const std::vector<std::uint8_t>& bytes, size_t max_bytes)
{
    std::fprintf(stdout, "[chafa-debug] %s: %zu bytes\n", label ? label : "(bytes)", bytes.size());
    const size_t n = std::min(bytes.size(), max_bytes);
    std::fprintf(stdout, "[chafa-debug] %s (escaped, first %zu bytes):\n", label ? label : "(bytes)", n);
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = (unsigned char)bytes[i];
        if (c == 0x1B) { std::fputs("\\x1b", stdout); continue; } // ESC
        if (c == '\n') { std::fputs("\\n\n", stdout); continue; } // newline + real newline
        if (c == '\r') { std::fputs("\\r", stdout); continue; }
        if (c == '\t') { std::fputs("\\t", stdout); continue; }
        if (c < 0x20 || c == 0x7F)
        {
            std::fprintf(stdout, "\\x%02X", (unsigned)c);
            continue;
        }
        std::fputc((int)c, stdout);
    }
    if (bytes.size() > n)
        std::fprintf(stdout, "\n[chafa-debug] ... truncated (%zu more bytes)\n", bytes.size() - n);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static void DebugPrintCanvasStats(const char* label, const AnsiCanvas& c)
{
    const int rows = c.GetRows();
    const int cols = c.GetColumns();
    size_t non_space = 0;
    size_t fg_set = 0;
    size_t bg_set = 0;

    for (int r = 0; r < rows; ++r)
    {
        for (int col = 0; col < cols; ++col)
        {
            AnsiCanvas::Color32 fg = 0, bg = 0;
            const char32_t cp = c.GetLayerCell(0, r, col);
            c.GetLayerCellColors(0, r, col, fg, bg);
            if (cp != U' ')
                non_space++;
            if (fg != 0)
                fg_set++;
            if (bg != 0)
                bg_set++;
        }
    }

    std::fprintf(stdout,
                 "[chafa-debug] %s: cols=%d rows=%d non_space=%zu fg_set=%zu bg_set=%zu\n",
                 label ? label : "(canvas)", cols, rows, non_space, fg_set, bg_set);
    std::fflush(stdout);
}

static void DebugPrintImageSamples(const ImageRgba& src)
{
    auto sample = [&](int x, int y)
    {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= src.width) x = src.width - 1;
        if (y >= src.height) y = src.height - 1;
        const size_t off = (size_t)y * (size_t)src.rowstride + (size_t)x * 4u;
        if (off + 3 >= src.pixels.size())
        {
            std::fprintf(stdout, "[chafa-debug] sample(%d,%d): out of range\n", x, y);
            return;
        }
        const unsigned r = src.pixels[off + 0];
        const unsigned g = src.pixels[off + 1];
        const unsigned b = src.pixels[off + 2];
        const unsigned a = src.pixels[off + 3];
        std::fprintf(stdout, "[chafa-debug] sample(%d,%d) RGBA=(%u,%u,%u,%u)\n", x, y, r, g, b, a);
    };

    std::fprintf(stdout, "[chafa-debug] src: w=%d h=%d rowstride=%d pixels=%zu\n",
                 src.width, src.height, src.rowstride, src.pixels.size());
    sample(0, 0);
    sample(src.width / 2, src.height / 2);
    sample(src.width - 1, src.height - 1);
    std::fflush(stdout);
}

static void DebugPrintChafaCanvasSamples(ChafaCanvas* canvas, int w, int h)
{
    if (!canvas || w <= 0 || h <= 0)
        return;

    auto sample = [&](int x, int y)
    {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        gunichar ch = chafa_canvas_get_char_at(canvas, x, y);
        gint fg_raw = -1, bg_raw = -1;
        chafa_canvas_get_raw_colors_at(canvas, x, y, &fg_raw, &bg_raw);
        std::fprintf(stdout, "[chafa-debug] canvas(%d,%d): ch=U+%04X fg_raw=%d bg_raw=%d\n",
                     x, y, (unsigned)ch, (int)fg_raw, (int)bg_raw);
    };

    sample(0, 0);
    sample(w / 2, h / 2);
    sample(w - 1, h - 1);
    std::fflush(stdout);
}

static ChafaDitherMode ToDitherMode(int ui_value)
{
    switch (ui_value)
    {
        case 0: return CHAFA_DITHER_MODE_NONE;
        case 1: return CHAFA_DITHER_MODE_ORDERED;
        case 2: return CHAFA_DITHER_MODE_DIFFUSION;
        case 3: return CHAFA_DITHER_MODE_NOISE;
        default: return CHAFA_DITHER_MODE_DIFFUSION;
    }
}

static ChafaCanvasMode ToCanvasMode(int ui_value)
{
    // UI order:
    // 0 Indexed 256, 1 Indexed 240, 2 Indexed 16, 3 Indexed 16/8, 4 Indexed 8, 5 Default fg/bg + invert, 6 Default fg/bg (no codes)
    switch (ui_value)
    {
        case 1: return CHAFA_CANVAS_MODE_INDEXED_240;
        case 2: return CHAFA_CANVAS_MODE_INDEXED_16;
        case 3: return CHAFA_CANVAS_MODE_INDEXED_16_8;
        case 4: return CHAFA_CANVAS_MODE_INDEXED_8;
        case 5: return CHAFA_CANVAS_MODE_FGBG_BGFG;
        case 6: return CHAFA_CANVAS_MODE_FGBG;
        case 0:
        default: return CHAFA_CANVAS_MODE_INDEXED_256;
    }
}

static ChafaColorExtractor ToColorExtractor(int ui_value)
{
    switch (ui_value)
    {
        case 1: return CHAFA_COLOR_EXTRACTOR_MEDIAN;
        case 0:
        default: return CHAFA_COLOR_EXTRACTOR_AVERAGE;
    }
}

static ChafaColorSpace ToColorSpace(int ui_value)
{
    switch (ui_value)
    {
        case 1: return CHAFA_COLOR_SPACE_DIN99D;
        case 0:
        default: return CHAFA_COLOR_SPACE_RGB;
    }
}

static ChafaSymbolTags PresetToSymbolTags(int preset)
{
    switch (preset)
    {
        case 0: return CHAFA_SYMBOL_TAG_ALL;
        case 1: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_QUAD |
                                         CHAFA_SYMBOL_TAG_SEXTANT | CHAFA_SYMBOL_TAG_OCTANT |
                                         CHAFA_SYMBOL_TAG_SOLID | CHAFA_SYMBOL_TAG_STIPPLE);
        case 2: return CHAFA_SYMBOL_TAG_ASCII;
        case 3: return CHAFA_SYMBOL_TAG_BRAILLE;
        default: return CHAFA_SYMBOL_TAG_ALL;
    }
}

static bool ApplySelectorsOrPreset(ChafaSymbolMap* map,
                                  const std::string& selectors,
                                  int preset,
                                  std::string& out_err)
{
    if (!map)
        return false;

    if (!selectors.empty())
    {
        GError* gerr = nullptr;
        const gboolean ok = chafa_symbol_map_apply_selectors(map, selectors.c_str(), &gerr);
        if (!ok)
        {
            if (gerr && gerr->message)
                out_err = gerr->message;
            else
                out_err = "Invalid symbol selectors.";
            if (gerr)
                g_error_free(gerr);
            return false;
        }
    }
    else
    {
        chafa_symbol_map_add_by_tags(map, PresetToSymbolTags(preset));
    }

    // Compatibility: sextant/octant are non-BMP Unicode (Symbols for Legacy Computing).
    // They are great for fidelity, but they are not part of CP437 and can render
    // inconsistently depending on the rendering stack. To keep output predictable:
    // - If the user provided explicit selectors, do NOT override them.
    // - Otherwise, for "All" and "Blocks" presets, exclude sextant+octant by default.
    if (selectors.empty() && (preset == 0 /* All */ || preset == 1 /* Blocks */))
        chafa_symbol_map_remove_by_tags(map, (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_SEXTANT | CHAFA_SYMBOL_TAG_OCTANT));

    return true;
}
} // namespace

bool ConvertRgbaToAnsiCanvas(const ImageRgba& src, const Settings& s, AnsiCanvas& out, std::string& out_err)
{
    out_err.clear();

    if (src.width <= 0 || src.height <= 0 || src.pixels.empty())
    {
        out_err = "No image data.";
        return false;
    }
    if (src.rowstride <= 0 || src.rowstride < src.width * 4)
    {
        out_err = "Invalid rowstride.";
        return false;
    }

    if (s.debug_stdout)
        DebugPrintImageSamples(src);

    // Compute output geometry.
    gint out_w = std::max(1, s.out_cols);
    // IMPORTANT: For chafa_calc_canvas_geometry(), a dimension of 0 means "explicitly zero",
    // which forces both outputs to 0. Use < 0 to mark an unspecified dimension.
    gint out_h = s.auto_rows ? -1 : std::max(1, s.out_rows);

    const gfloat font_ratio = std::clamp(s.font_ratio, 0.1f, 4.0f);
    chafa_calc_canvas_geometry((gint)src.width,
                              (gint)src.height,
                              &out_w,
                              &out_h,
                              font_ratio,
                              (gboolean)(s.zoom ? TRUE : FALSE),
                              (gboolean)(s.stretch ? TRUE : FALSE));

    if (out_w <= 0) out_w = 1;
    if (out_h <= 0) out_h = 1;

    // Apply thread count per conversion (libchafa uses global thread state).
    const gint prev_threads = chafa_get_n_threads();
    const gint want_threads = (gint)s.threads;
    if (want_threads != prev_threads)
        chafa_set_n_threads(want_threads);

    ChafaCanvasConfig* cfg = chafa_canvas_config_new();
    if (!cfg)
    {
        out_err = "chafa_canvas_config_new() failed.";
        if (want_threads != prev_threads)
            chafa_set_n_threads(prev_threads);
        return false;
    }

    // Ensure we always generate character art (not sixel/kitty/etc).
    chafa_canvas_config_set_pixel_mode(cfg, CHAFA_PIXEL_MODE_SYMBOLS);

    chafa_canvas_config_set_geometry(cfg, out_w, out_h);
    chafa_canvas_config_set_canvas_mode(cfg, ToCanvasMode(s.canvas_mode));
    chafa_canvas_config_set_color_extractor(cfg, ToColorExtractor(s.color_extractor));
    chafa_canvas_config_set_color_space(cfg, ToColorSpace(s.color_space));

    // Work factor:
    // - CLI uses --work [1..9]
    // - libchafa expects work_factor in [0.0 .. 1.0]
    // Map linearly: 1 -> 0.0, 9 -> 1.0.
    const int work_i = std::clamp(s.work, 1, 9);
    const float work_factor = (work_i <= 1) ? 0.0f : (float)(work_i - 1) / 8.0f;
    chafa_canvas_config_set_work_factor(cfg, work_factor);

    // Reduce the odds of emitting non-7-bit control sequences; we feed output into our ANSI importer.
    // This also makes the stream easier to debug.
    chafa_canvas_config_set_optimizations(cfg, CHAFA_OPTIMIZATION_NONE);

    chafa_canvas_config_set_preprocessing_enabled(cfg, (gboolean)(s.preprocessing ? TRUE : FALSE));

    // NOTE: Chafa's "transparency threshold" is inverted internally (it stores an opacity threshold).
    // Passing 0.0 maps to an internal alpha threshold of 256, which makes even fully-opaque (255) pixels
    // become transparent. Our UI semantics are: 0.0 = no extra transparency, 1.0 = everything transparent.
    const float ui_t = std::clamp(s.transparency_threshold, 0.0f, 1.0f);
    chafa_canvas_config_set_transparency_threshold(cfg, 1.0f - ui_t);

    // Dithering controls (mode + grain + intensity).
    chafa_canvas_config_set_dither_mode(cfg, ToDitherMode(s.dither_mode));
    const gint g = (gint)std::clamp(s.dither_grain, 1, 8);
    chafa_canvas_config_set_dither_grain_size(cfg, g, g);
    chafa_canvas_config_set_dither_intensity(cfg, std::max(0.0f, s.dither_intensity));

    chafa_canvas_config_set_fg_only_enabled(cfg, (gboolean)(s.fg_only ? TRUE : FALSE));

    if (s.use_custom_fg_bg)
    {
        uint32_t fg = s.fg_rgb & 0xFFFFFFu;
        uint32_t bg = s.bg_rgb & 0xFFFFFFu;
        if (s.invert_fg_bg)
            std::swap(fg, bg);
        chafa_canvas_config_set_fg_color(cfg, (guint32)fg);
        chafa_canvas_config_set_bg_color(cfg, (guint32)bg);
    }

    // Symbol selection (symbols + fill).
    ChafaSymbolMap* sym = chafa_symbol_map_new();
    ChafaSymbolMap* fill = chafa_symbol_map_new();
    if (sym && fill)
    {
        std::string sel_err;
        if (!ApplySelectorsOrPreset(sym, s.symbols_selectors, s.symbol_preset, sel_err))
        {
            out_err = sel_err.empty() ? "Invalid symbol selection." : sel_err;
            chafa_symbol_map_unref(sym);
            chafa_symbol_map_unref(fill);
            chafa_canvas_config_unref(cfg);
            if (want_threads != prev_threads)
                chafa_set_n_threads(prev_threads);
            return false;
        }

        // If fill selectors not specified, mirror symbols selection.
        if (s.fill_selectors.empty())
        {
            chafa_symbol_map_unref(fill);
            fill = chafa_symbol_map_copy(sym);
        }
        else
        {
            std::string fill_err;
            if (!ApplySelectorsOrPreset(fill, s.fill_selectors, s.symbol_preset, fill_err))
            {
                out_err = fill_err.empty() ? "Invalid fill selection." : fill_err;
                chafa_symbol_map_unref(sym);
                chafa_symbol_map_unref(fill);
                chafa_canvas_config_unref(cfg);
                if (want_threads != prev_threads)
                    chafa_set_n_threads(prev_threads);
                return false;
            }
        }

        chafa_canvas_config_set_symbol_map(cfg, sym);
        chafa_canvas_config_set_fill_symbol_map(cfg, fill);
    }
    if (sym) chafa_symbol_map_unref(sym);
    if (fill) chafa_symbol_map_unref(fill);

    ChafaCanvas* canvas = chafa_canvas_new(cfg);
    chafa_canvas_config_unref(cfg);
    cfg = nullptr;

    if (!canvas)
    {
        out_err = "chafa_canvas_new() failed.";
        if (want_threads != prev_threads)
            chafa_set_n_threads(prev_threads);
        return false;
    }

    chafa_canvas_draw_all_pixels(canvas,
                                 CHAFA_PIXEL_RGBA8_UNASSOCIATED,
                                 (const guint8*)src.pixels.data(),
                                 (gint)src.width,
                                 (gint)src.height,
                                 (gint)src.rowstride);

    if (s.debug_stdout)
        DebugPrintChafaCanvasSamples(canvas, (int)out_w, (int)out_h);

    // IMPORTANT: Chafa's printable output is UTF-8 + terminal escape sequences.
    // We intentionally run it through our ANSI importer so preview matches the "real" import path.
    GString* gs = chafa_canvas_print(canvas, nullptr);
    chafa_canvas_unref(canvas);

    if (want_threads != prev_threads)
        chafa_set_n_threads(prev_threads);

    if (!gs)
    {
        out_err = "chafa_canvas_print() failed.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    bytes.resize((size_t)gs->len);
    if (gs->len > 0 && gs->str)
        std::memcpy(bytes.data(), gs->str, (size_t)gs->len);

    if (s.debug_stdout)
    {
        std::fprintf(stdout, "[chafa-debug] chafa_canvas_print: len=%u\n", (unsigned)gs->len);
        if (s.debug_dump_raw_ansi)
        {
            std::fputs("[chafa-debug] RAW ANSI START\n", stdout);
            if (gs->len > 0 && gs->str)
                std::fwrite(gs->str, 1, (size_t)gs->len, stdout);
            std::fputs("\n[chafa-debug] RAW ANSI END\n", stdout);
        }
        DebugPrintEscapedAnsiBytes("chafa_output", bytes, 4096);
    }
    g_string_free(gs, TRUE);

    formats::ansi::ImportOptions opt;
    opt.columns = (int)out_w;
    // Force UTF-8 decoding even though the stream contains ESC sequences.
    // Chafa's docs guarantee UTF-8 output regardless of locale.
    opt.cp437 = false;
    // Don't force an opaque default background for generated output.
    opt.default_bg_unset = true;
    // Avoid libansilove-style eager wrap for generated output; chafa may emit explicit
    // newlines at the row boundary, which would double-advance with eager wrapping.
    opt.wrap_policy = formats::ansi::ImportOptions::WrapPolicy::PutOnly;

    AnsiCanvas imported;
    std::string ierr;
    if (!formats::ansi::ImportBytesToCanvas(bytes, imported, ierr, opt))
    {
        out_err = ierr.empty() ? "ANSI import failed." : ierr;
        return false;
    }

    if (s.debug_stdout)
        DebugPrintCanvasStats("imported_preview", imported);

    out = std::move(imported);
    return true;
}
} // namespace chafa_convert


