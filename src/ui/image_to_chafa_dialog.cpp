// Image -> Chafa conversion dialog implementation.

#include "ui/image_to_chafa_dialog.h"

#include "io/ansi_importer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
// Chafa (C library)
#include <chafa.h>

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
        if (c == 0x1B) { std::fputs("\\x1b", stdout); continue; }     // ESC
        if (c == '\n') { std::fputs("\\n\n", stdout); continue; }     // newline + real newline for readability
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

static void DebugPrintImageSamples(const ImageToChafaDialog::ImageRgba& src)
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
    switch (ui_value)
    {
        case 0: return CHAFA_CANVAS_MODE_INDEXED_256;
        case 1: return CHAFA_CANVAS_MODE_TRUECOLOR;
        default: return CHAFA_CANVAS_MODE_INDEXED_256;
    }
}

static ChafaSymbolTags ToSymbolTags(int preset)
{
    switch (preset)
    {
        // NOTE: We intentionally exclude SPACE from the symbol set for the preview.
        // Chafa can represent pixels using "colored spaces" (foreground only), but our
        // renderer skips drawing spaces unless they have a background fill. Excluding
        // SPACE forces visible glyph output (blocks/braille/etc).
        case 0: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_ALL & ~CHAFA_SYMBOL_TAG_SPACE);
        case 1: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_QUAD |
                                         CHAFA_SYMBOL_TAG_SEXTANT | CHAFA_SYMBOL_TAG_OCTANT |
                                         CHAFA_SYMBOL_TAG_SOLID | CHAFA_SYMBOL_TAG_STIPPLE);
        case 2: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_ASCII);
        case 3: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_BRAILLE);
        default: return CHAFA_SYMBOL_TAG_ALL;
    }
}

static bool ConvertRgbaToAnsiCanvas(const ImageToChafaDialog::ImageRgba& src,
                                   const ImageToChafaDialog::Settings& s,
                                   AnsiCanvas& out,
                                   std::string& out_err)
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

    ChafaCanvasConfig* cfg = chafa_canvas_config_new();
    if (!cfg)
    {
        out_err = "chafa_canvas_config_new() failed.";
        return false;
    }

    // Ensure we always generate character art (not sixel/kitty/etc).
    chafa_canvas_config_set_pixel_mode(cfg, CHAFA_PIXEL_MODE_SYMBOLS);

    chafa_canvas_config_set_geometry(cfg, out_w, out_h);
    chafa_canvas_config_set_canvas_mode(cfg, ToCanvasMode(s.canvas_mode));
    chafa_canvas_config_set_preprocessing_enabled(cfg, (gboolean)(s.preprocessing ? TRUE : FALSE));
    // NOTE: Chafa's "transparency threshold" is inverted internally (it stores an opacity threshold).
    // Passing 0.0 maps to an internal alpha threshold of 256, which makes even fully-opaque (255) pixels
    // become transparent. Our UI semantics are: 0.0 = no extra transparency, 1.0 = everything transparent.
    const float ui_t = std::clamp(s.transparency_threshold, 0.0f, 1.0f);
    chafa_canvas_config_set_transparency_threshold(cfg, 1.0f - ui_t);

    // Dithering controls (mode + intensity).
    chafa_canvas_config_set_dither_mode(cfg, ToDitherMode(s.dither_mode));
    chafa_canvas_config_set_dither_intensity(cfg, std::clamp(s.dither_intensity, 0.0f, 1.0f));

    // Symbol selection.
    ChafaSymbolMap* sym = chafa_symbol_map_new();
    if (sym)
    {
        // IMPORTANT: Chafa uses a separate "fill symbol map" for choosing fill characters.
        // If it's empty, some configurations degenerate into SPACE-only output. We keep
        // symbol_map and fill_symbol_map in sync for predictable results.
        chafa_symbol_map_add_by_tags(sym, ToSymbolTags(s.symbol_preset));
        // Be explicit: keep spaces available for padding, but don't allow them as a chosen symbol.
        chafa_symbol_map_remove_by_tags(sym, CHAFA_SYMBOL_TAG_SPACE);
        chafa_canvas_config_set_symbol_map(cfg, sym);
        chafa_canvas_config_set_fill_symbol_map(cfg, sym);
        chafa_symbol_map_unref(sym);
    }

    ChafaCanvas* canvas = chafa_canvas_new(cfg);
    chafa_canvas_config_unref(cfg);
    cfg = nullptr;

    if (!canvas)
    {
        out_err = "chafa_canvas_new() failed.";
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
        std::fprintf(stdout,
                     "[chafa-debug] chafa_canvas_print: len=%u\n",
                     (unsigned)gs->len);
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

    ansi_importer::Options opt;
    opt.columns = (int)out_w;
    // Force UTF-8 decoding even though the stream contains ESC sequences.
    // Chafa's docs guarantee UTF-8 output regardless of locale.
    opt.cp437 = false;
    // Don't force an opaque default background for generated output.
    opt.default_bg_unset = true;
    // Avoid libansilove-style eager wrap for generated output; chafa may emit explicit
    // newlines at the row boundary, which would double-advance with eager wrapping.
    opt.wrap_policy = ansi_importer::Options::WrapPolicy::PutOnly;

    AnsiCanvas imported;
    std::string ierr;
    if (!ansi_importer::ImportAnsiBytesToCanvas(bytes, imported, ierr, opt))
    {
        out_err = ierr.empty() ? "ANSI import failed." : ierr;
        return false;
    }

    if (s.debug_stdout)
        DebugPrintCanvasStats("imported_preview", imported);

    out = std::move(imported);
    return true;
}
} // namespace

void ImageToChafaDialog::Open(ImageRgba src)
{
    src_ = std::move(src);
    if (src_.rowstride <= 0)
        src_.rowstride = src_.width * 4;

    open_ = true;
    open_popup_next_frame_ = true;
    dirty_ = true;
    error_.clear();
    has_preview_ = false;
}

bool ImageToChafaDialog::RegeneratePreview()
{
    AnsiCanvas next;
    std::string err;
    if (!ConvertRgbaToAnsiCanvas(src_, settings_, next, err))
    {
        error_ = err.empty() ? "Conversion failed." : err;
        has_preview_ = false;
        return false;
    }

    preview_ = std::move(next);
    has_preview_ = true;
    error_.clear();
    return true;
}

void ImageToChafaDialog::Render()
{
    if (!open_)
        return;

    if (open_popup_next_frame_)
    {
        ImGui::OpenPopup("Convert Image to ANSI");
        open_popup_next_frame_ = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::BeginPopupModal("Convert Image to ANSI", &open_, flags))
        return;

    // Settings UI (left) + Preview (right).
    ImGui::Text("Source: %s", src_.label.empty() ? "(image)" : src_.label.c_str());
    ImGui::Text("Size: %dx%d", src_.width, src_.height);
    ImGui::Separator();

    bool changed = false;

    ImGui::BeginGroup();
    {
        changed |= ImGui::InputInt("Columns", &settings_.out_cols);
        settings_.out_cols = std::clamp(settings_.out_cols, 1, 400);

        changed |= ImGui::Checkbox("Auto rows", &settings_.auto_rows);
        if (!settings_.auto_rows)
        {
            changed |= ImGui::InputInt("Rows", &settings_.out_rows);
            settings_.out_rows = std::clamp(settings_.out_rows, 1, 400);
        }
        else
        {
            ImGui::TextDisabled("Rows: auto");
        }

        changed |= ImGui::SliderFloat("Font ratio (w/h)", &settings_.font_ratio, 0.2f, 2.0f, "%.3f");
        changed |= ImGui::Checkbox("Zoom", &settings_.zoom);
        changed |= ImGui::Checkbox("Stretch", &settings_.stretch);

        const char* mode_items[] = {"Indexed 256 (xterm)", "Truecolor"};
        changed |= ImGui::Combo("Color mode", &settings_.canvas_mode, mode_items, IM_ARRAYSIZE(mode_items));

        const char* sym_items[] = {"All", "Blocks", "ASCII", "Braille"};
        changed |= ImGui::Combo("Symbols", &settings_.symbol_preset, sym_items, IM_ARRAYSIZE(sym_items));

        const char* dither_items[] = {"None", "Ordered", "Diffusion", "Noise"};
        changed |= ImGui::Combo("Dither", &settings_.dither_mode, dither_items, IM_ARRAYSIZE(dither_items));
        changed |= ImGui::SliderFloat("Dither intensity", &settings_.dither_intensity, 0.0f, 1.0f, "%.2f");

        changed |= ImGui::Checkbox("Preprocessing", &settings_.preprocessing);
        changed |= ImGui::SliderFloat("Transparency threshold", &settings_.transparency_threshold, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        changed |= ImGui::Checkbox("Debug to stdout", &settings_.debug_stdout);
        if (settings_.debug_stdout)
        {
            changed |= ImGui::Checkbox("Dump RAW ANSI (danger)", &settings_.debug_dump_raw_ansi);
            ImGui::TextDisabled("Tip: keep RAW off; use escaped dump to avoid terminal garbage.");
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    {
        ImGui::TextUnformatted("Preview");
        ImGui::Separator();

        if (changed)
            dirty_ = true;
        if (dirty_)
        {
            RegeneratePreview();
            dirty_ = false;
        }

        if (!error_.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
        }

        ImVec2 preview_size(720.0f, 420.0f);
        if (ImGui::BeginChild("chafa_preview_child", preview_size, true, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (has_preview_)
                preview_.Render("chafa_preview_canvas", std::function<void(AnsiCanvas&, int)>{});
            else
                ImGui::TextUnformatted("(no preview)");
            ImGui::EndChild();
        }
    }
    ImGui::EndGroup();

    ImGui::Separator();

    const bool can_accept = has_preview_ && error_.empty();
    if (!can_accept)
        ImGui::BeginDisabled();
    if (ImGui::Button("OK"))
    {
        accepted_canvas_ = std::move(preview_);
        accepted_ = true;
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (!can_accept)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

bool ImageToChafaDialog::TakeAccepted(AnsiCanvas& out)
{
    if (!accepted_)
        return false;
    out = std::move(accepted_canvas_);
    accepted_ = false;
    return true;
}


