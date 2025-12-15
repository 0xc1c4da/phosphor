// Image -> Chafa conversion dialog implementation.

#include "image_to_chafa_dialog.h"

#include "imgui.h"
#include "xterm256_palette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Chafa (C library)
#include <chafa.h>

namespace
{
static inline AnsiCanvas::Color32 PackImGuiCol32(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Matches Dear ImGui's IM_COL32(R,G,B,A) packing: A in high byte, then B,G,R.
    return 0xFF000000u | ((std::uint32_t)b << 16) | ((std::uint32_t)g << 8) | (std::uint32_t)r;
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
        case 0: return CHAFA_SYMBOL_TAG_ALL;
        case 1: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_QUAD |
                                         CHAFA_SYMBOL_TAG_SEXTANT | CHAFA_SYMBOL_TAG_OCTANT |
                                         CHAFA_SYMBOL_TAG_SOLID | CHAFA_SYMBOL_TAG_STIPPLE | CHAFA_SYMBOL_TAG_SPACE);
        case 2: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_ASCII | CHAFA_SYMBOL_TAG_SPACE);
        case 3: return (ChafaSymbolTags)(CHAFA_SYMBOL_TAG_BRAILLE | CHAFA_SYMBOL_TAG_SPACE);
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
    chafa_canvas_config_set_transparency_threshold(cfg, std::clamp(s.transparency_threshold, 0.0f, 1.0f));

    // Dithering controls (mode + intensity).
    chafa_canvas_config_set_dither_mode(cfg, ToDitherMode(s.dither_mode));
    chafa_canvas_config_set_dither_intensity(cfg, std::clamp(s.dither_intensity, 0.0f, 1.0f));

    // Symbol selection.
    ChafaSymbolMap* sym = chafa_symbol_map_new();
    if (sym)
    {
        chafa_symbol_map_add_by_tags(sym, ToSymbolTags(s.symbol_preset));
        chafa_canvas_config_set_symbol_map(cfg, sym);
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

    // Build AnsiCanvas output.
    out = AnsiCanvas((int)out_w);
    out.EnsureRowsPublic((int)out_h);
    out.ClearLayer(0, U' ');

    const ChafaCanvasMode mode = ToCanvasMode(s.canvas_mode);
    for (gint y = 0; y < out_h; ++y)
    {
        for (gint x = 0; x < out_w; ++x)
        {
            gunichar ch = chafa_canvas_get_char_at(canvas, x, y);
            if (ch == 0)
                ch = (gunichar)' ';

            gint fg_raw = -1, bg_raw = -1;
            chafa_canvas_get_raw_colors_at(canvas, x, y, &fg_raw, &bg_raw);

            AnsiCanvas::Color32 fg = 0;
            AnsiCanvas::Color32 bg = 0;

            if (mode == CHAFA_CANVAS_MODE_TRUECOLOR)
            {
                if (fg_raw >= 0)
                {
                    const std::uint8_t r = (std::uint8_t)((fg_raw >> 16) & 0xFF);
                    const std::uint8_t g = (std::uint8_t)((fg_raw >> 8) & 0xFF);
                    const std::uint8_t b = (std::uint8_t)(fg_raw & 0xFF);
                    fg = PackImGuiCol32(r, g, b);
                }
                if (bg_raw >= 0)
                {
                    const std::uint8_t r = (std::uint8_t)((bg_raw >> 16) & 0xFF);
                    const std::uint8_t g = (std::uint8_t)((bg_raw >> 8) & 0xFF);
                    const std::uint8_t b = (std::uint8_t)(bg_raw & 0xFF);
                    bg = PackImGuiCol32(r, g, b);
                }
            }
            else
            {
                if (fg_raw >= 0)
                    fg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(fg_raw);
                if (bg_raw >= 0)
                    bg = (AnsiCanvas::Color32)xterm256::Color32ForIndex(bg_raw);
            }

            out.SetLayerCell(0, (int)y, (int)x, (char32_t)ch, fg, bg);
        }
    }

    chafa_canvas_unref(canvas);
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


