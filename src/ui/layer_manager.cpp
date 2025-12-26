#include "ui/layer_manager.h"

#include "core/canvas.h"
#include "core/color_system.h"
#include "core/fonts.h"
#include "core/glyph_resolve.h"
#include "core/i18n.h"
#include "imgui.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>

static float GetStableCellAspect(const AnsiCanvas& canvas)
{
    // Match minimap logic: prefer captured base metrics (stable across zoom), otherwise fall back.
    const AnsiCanvas::ViewState& vs = canvas.GetLastViewState();
    if (vs.valid && vs.base_cell_h > 0.0f && vs.base_cell_w > 0.0f)
    {
        const float a = vs.base_cell_w / vs.base_cell_h;
        if (a > 0.0f)
            return a;
    }
    // Reasonable default for most terminal-ish fonts.
    return 0.5f;
}

static void DrawCheckerboard(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float tile_px,
                             ImU32 a = IM_COL32(60, 60, 66, 255),
                             ImU32 b = IM_COL32(38, 38, 44, 255))
{
    if (!dl)
        return;
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    if (!(w > 1.0f) || !(h > 1.0f))
        return;

    tile_px = std::clamp(tile_px, 2.0f, 16.0f);
    const int nx = std::max(1, (int)std::ceil(w / tile_px));
    const int ny = std::max(1, (int)std::ceil(h / tile_px));

    for (int y = 0; y < ny; ++y)
    {
        for (int x = 0; x < nx; ++x)
        {
            const ImU32 col = ((x ^ y) & 1) ? a : b;
            const ImVec2 a0(p0.x + (float)x * tile_px, p0.y + (float)y * tile_px);
            const ImVec2 a1(std::min(p1.x, a0.x + tile_px), std::min(p1.y, a0.y + tile_px));
            dl->AddRectFilled(a0, a1, col);
        }
    }
}

static void ComputeLayerThumbnailGrid(const AnsiCanvas& canvas,
                                      int layer_index,
                                      int& out_gw,
                                      int& out_gh,
                                      std::vector<std::uint32_t>& out_colors)
{
    const int cols = canvas.GetColumns();
    const int rows = canvas.GetRows();
    if (cols <= 0 || rows <= 0)
    {
        out_gw = 0;
        out_gh = 0;
        out_colors.clear();
        return;
    }

    auto unpack = [](ImU32 c, int& r, int& g, int& b, int& a) {
        r = (int)(c & 0xFF);
        g = (int)((c >> 8) & 0xFF);
        b = (int)((c >> 16) & 0xFF);
        a = (int)((c >> 24) & 0xFF);
    };
    auto pack = [](int r, int g, int b, int a) -> ImU32 {
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);
        a = std::clamp(a, 0, 255);
        return (ImU32)((a << 24) | (b << 16) | (g << 8) | (r));
    };

    struct Ink2x2
    {
        float q00 = 0.0f; // top-left
        float q10 = 0.0f; // top-right
        float q01 = 0.0f; // bottom-left
        float q11 = 0.0f; // bottom-right
    };

    const fonts::FontId font_id = canvas.GetFontId();
    const fonts::FontInfo& finfo = fonts::Get(font_id);
    const AnsiCanvas::EmbeddedBitmapFont* ef = canvas.GetEmbeddedFont();
    const bool embedded_font_ok =
        (ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
         ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h);
    const bool bitmap_font =
        embedded_font_ok ||
        (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0);

    // Glyph ink cache (per process). Key includes font id.
    static std::unordered_map<std::uint64_t, Ink2x2> s_ink_cache;
    auto glyph_ink2x2 = [&](phos::GlyphId glyph) -> Ink2x2
    {
        Ink2x2 out{};
        if (phos::glyph::IsBlank((phos::GlyphId)glyph))
            return out;

        // Cache key: include embedded-font identity when present so thumbnails don't cross-contaminate
        // between different canvases' embedded fonts.
        std::uint64_t key_hi = 0;
        if (embedded_font_ok)
        {
            const std::uint64_t ef_tag = (std::uint64_t)((uintptr_t)ef >> 4); // drop alignment zeros
            key_hi = (0x80000000ull | (ef_tag & 0x7FFFFFFFull));
        }
        else
        {
            key_hi = (std::uint64_t)finfo.id;
        }
        const std::uint64_t key = (key_hi << 32) | (std::uint32_t)glyph;
        if (auto it = s_ink_cache.find(key); it != s_ink_cache.end())
            return it->second;

        if (!bitmap_font)
        {
            // Best effort: treat as solid fg so characters are visible.
            out.q00 = out.q10 = out.q01 = out.q11 = 1.0f;
            s_ink_cache.emplace(key, out);
            return out;
        }

        const phos::glyph::BitmapGlyphRef gr =
            phos::glyph::ResolveBitmapGlyph(finfo, embedded_font_ok ? ef : nullptr, (phos::GlyphId)glyph);
        const std::uint16_t glyph_index = gr.glyph_index;

        int w = finfo.cell_w;
        int h = finfo.cell_h;
        bool vga_dup = finfo.vga_9col_dup;
        if (embedded_font_ok)
        {
            w = ef->cell_w;
            h = ef->cell_h;
            vga_dup = ef->vga_9col_dup;
        }
        const int mid_x = w / 2;
        const int mid_y = h / 2;

        auto glyph_row_bits = [&](std::uint16_t gi, int yy) -> std::uint8_t
        {
            if (embedded_font_ok && gr.use_embedded)
            {
                if (gi >= (std::uint16_t)ef->glyph_count) return 0;
                if (yy < 0 || yy >= ef->cell_h) return 0;
                return ef->bitmap[(size_t)gi * (size_t)ef->cell_h + (size_t)yy];
            }
            return fonts::BitmapGlyphRowBits(finfo.id, gi, yy);
        };

        std::uint64_t sum00 = 0, cnt00 = 0;
        std::uint64_t sum10 = 0, cnt10 = 0;
        std::uint64_t sum01 = 0, cnt01 = 0;
        std::uint64_t sum11 = 0, cnt11 = 0;

        for (int yy = 0; yy < h; ++yy)
        {
            const std::uint8_t bits = glyph_row_bits(glyph_index, yy);
            for (int xx = 0; xx < w; ++xx)
            {
                bool on = false;
                if (xx < 8)
                    on = (bits & (std::uint8_t)(0x80u >> xx)) != 0;
                else if (xx == 8 && vga_dup && w == 9 && glyph_index >= 192 && glyph_index <= 223)
                    on = (bits & 0x01u) != 0;

                const std::uint64_t a = on ? 255u : 0u;
                const bool right = (xx >= mid_x);
                const bool bot = (yy >= mid_y);
                if (!right && !bot) { sum00 += a; ++cnt00; }
                else if (right && !bot) { sum10 += a; ++cnt10; }
                else if (!right && bot) { sum01 += a; ++cnt01; }
                else { sum11 += a; ++cnt11; }
            }
        }

        auto cov = [](std::uint64_t sum, std::uint64_t cnt) -> float
        {
            if (cnt == 0)
                return 0.0f;
            const float v = (float)sum / (float)(cnt * 255.0);
            return std::clamp(v, 0.0f, 1.0f);
        };

        out.q00 = cov(sum00, cnt00);
        out.q10 = cov(sum10, cnt10);
        out.q01 = cov(sum01, cnt01);
        out.q11 = cov(sum11, cnt11);
        s_ink_cache.emplace(key, out);
        return out;
    };

    // Coarse sampling into a small grid so thumbnails stay cheap.
    const float aspect = GetStableCellAspect(canvas);
    const float src_w_units = (float)cols * aspect;
    const float src_h_units = (float)rows;
    const float ratio = (src_h_units > 0.0f) ? (src_w_units / src_h_units) : 1.0f;

    const int max_dim = 56; // tuned for list usage: crisp enough, still fast
    int gw = 1, gh = 1;
    if (ratio >= 1.0f)
    {
        gw = max_dim;
        gh = std::max(1, (int)std::lround((double)max_dim / (double)ratio));
    }
    else
    {
        gh = max_dim;
        gw = std::max(1, (int)std::lround((double)max_dim * (double)ratio));
    }

    gw = std::clamp(gw, 1, max_dim);
    gh = std::clamp(gh, 1, max_dim);

    out_gw = gw;
    out_gh = gh;
    out_colors.assign((size_t)gw * (size_t)gh, 0u);

    // Defaults when fg is unset.
    const ImU32 default_fg = canvas.IsCanvasBackgroundWhite() ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

    for (int y = 0; y < gh; ++y)
    {
        const float fy = ((y + 0.5f) * (float)rows) / (float)gh;
        const int src_row = std::clamp((int)std::floor(fy), 0, rows - 1);
        const float ly = std::clamp(fy - (float)src_row, 0.0f, 1.0f);

        for (int x = 0; x < gw; ++x)
        {
            // Map x in "aspect-adjusted" units back to canvas columns.
            const float u_units = ((x + 0.5f) / (float)gw) * src_w_units;
            const float fx = (aspect > 0.0f) ? (u_units / aspect) : 0.0f;
            const int src_col = std::clamp((int)std::floor(fx), 0, cols - 1);
            const float lx = std::clamp(fx - (float)src_col, 0.0f, 1.0f);

            const AnsiCanvas::GlyphId glyph = canvas.GetLayerGlyph(layer_index, src_row, src_col);
            AnsiCanvas::ColorIndex16 fi = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColorIndex16 bi = AnsiCanvas::kUnsetIndex16;
            (void)canvas.GetLayerCellIndices(layer_index, src_row, src_col, fi, bi);

            auto& cs = phos::color::GetColorSystem();
            phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm256);
            if (auto id = cs.Palettes().Resolve(canvas.GetPaletteRef()))
                pal = *id;
            const AnsiCanvas::Color32 fg = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal, phos::color::ColorIndex{fi});
            const AnsiCanvas::Color32 bg = (AnsiCanvas::Color32)phos::color::ColorOps::IndexToColor32(cs.Palettes(), pal, phos::color::ColorIndex{bi});

            if (bg == 0 && phos::glyph::IsBlank((phos::GlyphId)glyph))
                continue;

            const ImU32 bg_col = (bg != 0) ? (ImU32)bg : IM_COL32(0, 0, 0, 0); // transparent
            const ImU32 fg_col = (fg != 0) ? (ImU32)fg : default_fg;

            // Approximate glyph coverage (0..1) and blend fg over bg.
            const Ink2x2 ink = glyph_ink2x2((phos::GlyphId)glyph);
            const bool right = (lx >= 0.5f);
            const bool bot   = (ly >= 0.5f);
            float t = 0.0f;
            if (!right && !bot) t = ink.q00;
            else if (right && !bot) t = ink.q10;
            else if (!right && bot) t = ink.q01;
            else t = ink.q11;

            // Sharpen a bit so thin outlines survive downscale.
            const float sharp = 1.6f;
            t = std::clamp((t - 0.5f) * sharp + 0.5f, 0.0f, 1.0f);

            if (bg != 0)
            {
                int br, bgc, bb, ba;
                int fr, fgcc, fb, fa;
                unpack(bg_col, br, bgc, bb, ba);
                unpack(fg_col, fr, fgcc, fb, fa);
                const int r = (int)std::lround((double)br + ((double)fr - (double)br) * (double)t);
                const int g = (int)std::lround((double)bgc + ((double)fgcc - (double)bgc) * (double)t);
                const int b = (int)std::lround((double)bb + ((double)fb - (double)bb) * (double)t);
                out_colors[(size_t)y * (size_t)gw + (size_t)x] = (std::uint32_t)pack(r, g, b, 255);
            }
            else
            {
                // Transparent background: draw fg with alpha proportional to ink coverage so the checkerboard shows through.
                if (t <= 0.0f)
                    continue;
                int r, g, b, a;
                unpack(fg_col, r, g, b, a);
                const int aa = (int)std::lround(255.0 * (double)t);
                out_colors[(size_t)y * (size_t)gw + (size_t)x] = (std::uint32_t)pack(r, g, b, aa);
            }
        }
    }
}

static void DrawLayerThumbnailFromGrid(const std::vector<std::uint32_t>& colors,
                                       int gw,
                                       int gh,
                                       const ImVec2& size,
                                       bool dim)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Reserve item space without capturing input; the row's Selectable should receive clicks/drags.
    ImGui::Dummy(size);

    // Background frame + checkerboard for transparency.
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 3.0f);
    const float pad = 2.0f;
    const ImVec2 i0(p0.x + pad, p0.y + pad);
    const ImVec2 i1(p1.x - pad, p1.y - pad);
    DrawCheckerboard(dl, i0, i1, 6.0f);

    if (gw <= 0 || gh <= 0 || colors.empty())
    {
        dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 3.0f);
        return;
    }

    const float iw = std::max(1.0f, i1.x - i0.x);
    const float ih = std::max(1.0f, i1.y - i0.y);
    const float cw = iw / (float)gw;
    const float ch = ih / (float)gh;

    const size_t expected = (size_t)gw * (size_t)gh;
    const size_t n = std::min(expected, colors.size());
    for (int y = 0; y < gh; ++y)
    {
        const float y0 = i0.y + (float)y * ch;
        const float y1 = y0 + ch;
        for (int x = 0; x < gw; ++x)
        {
            const size_t idx = (size_t)y * (size_t)gw + (size_t)x;
            if (idx >= n)
                break;
            const ImU32 col = (ImU32)colors[idx];
            if (col == 0)
                continue;
            const float x0 = i0.x + (float)x * cw;
            const float x1 = x0 + cw;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }

    if (dim)
        dl->AddRectFilled(i0, i1, IM_COL32(0, 0, 0, 120), 0.0f);

    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 3.0f);
}

void LayerManager::Render(const char* title,
                          bool* p_open,
                          AnsiCanvas* active_canvas,
                          SessionState* session,
                          bool apply_placement_this_frame,
                          bool allow_thumbnail_refresh)
{
    if (!p_open || !*p_open)
        return;

    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    const std::string win_title = PHOS_TR("menu.window.layer_manager") + "##" + std::string(title);
    if (!ImGui::Begin(win_title.c_str(), p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, title);
        RenderImGuiWindowChromeMenu(session, title);
    }

    if (!active_canvas)
    {
        ImGui::TextUnformatted(PHOS_TR("layer_manager.no_active_canvas").c_str());
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }

    AnsiCanvas* canvas = active_canvas;
    if (thumb_cache_canvas_ != canvas)
    {
        thumb_cache_canvas_ = canvas;
        thumb_cache_.clear();
    }

    ImGui::Separator();

    const int layer_count = canvas->GetLayerCount();
    if (layer_count <= 0)
    {
        ImGui::TextUnformatted(PHOS_TR("layer_manager.no_layers_unexpected").c_str());
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return;
    }

    int active = canvas->GetActiveLayerIndex();
    if (active < 0) active = 0;
    if (active >= layer_count) active = layer_count - 1;

    if ((int)thumb_cache_.size() != layer_count)
        thumb_cache_.resize((size_t)layer_count);

    // If the active canvas changed, cancel any inline rename to avoid dangling pointer / wrong target.
    if (inline_rename_canvas_ && inline_rename_canvas_ != canvas)
    {
        inline_rename_canvas_ = nullptr;
        inline_rename_layer_index_ = -1;
        inline_rename_request_focus_ = false;
        inline_rename_buf_[0] = '\0';
    }

    // Header controls (apply to active layer unless stated).
    if (ImGui::Button(PHOS_TR("layer_manager.add_layer").c_str()))
        canvas->AddLayer("");
    ImGui::SameLine();
    if (ImGui::Button(PHOS_TR("layer_manager.remove_layer").c_str()))
        canvas->RemoveLayer(canvas->GetActiveLayerIndex());
    ImGui::SameLine();
    if (ImGui::Button(PHOS_TR("layer_manager.rename_ellipsis").c_str()))
    {
        rename_target_canvas_ = canvas;
        rename_target_layer_index_ = canvas->GetActiveLayerIndex();

        const std::string current = canvas->GetLayerName(rename_target_layer_index_);
        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", current.c_str());

        // Use a stable popup name but a unique ID scope per invocation.
        // This avoids ID mismatches between OpenPopup() and BeginPopupModal().
        rename_popup_serial_++;
        rename_popup_active_serial_ = rename_popup_serial_;
        rename_popup_requested_open_ = true;
    }

    // Open the popup when requested (must be done in the same ID scope as BeginPopupModal).
    if (rename_popup_requested_open_)
    {
        ImGui::PushID(rename_popup_active_serial_);
        const std::string rename_modal = PHOS_TR("layer_manager.rename_layer_modal") + "##rename_layer_modal";
        ImGui::OpenPopup(rename_modal.c_str());
        ImGui::PopID();
        rename_popup_requested_open_ = false;
    }

    // Always try to render the modal for the active rename serial; if it's not open, BeginPopupModal returns false.
    ImGui::PushID(rename_popup_active_serial_);
    {
        const std::string rename_modal = PHOS_TR("layer_manager.rename_layer_modal") + "##rename_layer_modal";
        if (ImGui::BeginPopupModal(rename_modal.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Verify the target canvas still exists this frame (avoid dangling pointer).
        const bool target_alive = (rename_target_canvas_ != nullptr) && (rename_target_canvas_ == active_canvas);
        if (!target_alive)
        {
            ImGui::TextUnformatted(PHOS_TR("layer_manager.target_canvas_missing").c_str());
        }
        else
        {
            const std::string s =
                PHOS_TRF("layer_manager.layer_name_fmt", phos::i18n::Arg::I64((long long)rename_target_layer_index_)) + ":";
            ImGui::TextUnformatted(s.c_str());
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##rename_layer_name", rename_buf_, IM_ARRAYSIZE(rename_buf_));
        }

        if (ImGui::Button(PHOS_TR("common.ok").c_str()))
        {
            if (target_alive && rename_target_layer_index_ >= 0)
                rename_target_canvas_->SetLayerName(rename_target_layer_index_, std::string(rename_buf_));
            rename_target_canvas_ = nullptr;
            rename_target_layer_index_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(PHOS_TR("common.cancel").c_str()))
        {
            rename_target_canvas_ = nullptr;
            rename_target_layer_index_ = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    }
    ImGui::PopID();

    ImGui::SeparatorText(PHOS_TR("layer_manager.layers_header").c_str());

    // Standard art-editor UX:
    // - Top of list = front (higher layer index).
    // - Click row to activate.
    // - Drag/drop rows to reorder (updates z-order).
    const float thumb_h = 42.0f;
    const float pad_y = 4.0f;
    const float line_h = ImGui::GetTextLineHeight();
    const float frame_h = ImGui::GetFrameHeight();
    // Layout:
    // - Line 1: name
    // - Line 2: visible + lock transparency
    // - Line 3: blend mode + opacity
    const float row_h = std::max(thumb_h + pad_y * 2.0f,
                                 pad_y + line_h + 2.0f + frame_h + 2.0f + frame_h + pad_y);

    // Use a scrollable child and clip rows so thumbnails are only drawn for visible items.
    if (ImGui::BeginChild("##layers_list", ImVec2(0, 0), true))
    {
        ImGuiListClipper clipper;
        clipper.Begin(layer_count, row_h);
        while (clipper.Step())
        {
            for (int display_i = clipper.DisplayStart; display_i < clipper.DisplayEnd; ++display_i)
            {
                const int layer_index = (layer_count - 1) - display_i; // display order: top=front
                const bool is_active = (layer_index == canvas->GetActiveLayerIndex());

                const bool is_visible = canvas->IsLayerVisible(layer_index);
                const bool is_locked = canvas->IsLayerTransparencyLocked(layer_index);

                const std::string raw_name = canvas->GetLayerName(layer_index);
                const std::string display_name = raw_name.empty() ? PHOS_TR("common.unnamed") : raw_name;

                ImGui::PushID(layer_index);

                // Row selectable (spans across) – other widgets are allowed to overlap.
                const ImGuiSelectableFlags sel_flags =
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap;
                if (ImGui::Selectable("##layer_row", is_active, sel_flags, ImVec2(0.0f, row_h)))
                    canvas->SetActiveLayerIndex(layer_index);

                // IMPORTANT: the Selectable already advanced the cursor to the next line.
                // We will temporarily place widgets over the row using SetCursorScreenPos(),
                // but then restore the cursor to this position to avoid extending bounds
                // (ImGui asserts if SetCursorPos is used to extend parent without an item).
                const ImVec2 cursor_after_row = ImGui::GetCursorScreenPos();

                // Context menu should work on the whole row.
                if (ImGui::BeginPopupContextItem("##layer_ctx"))
                {
                    if (ImGui::MenuItem(PHOS_TR("layer_manager.ctx_set_active").c_str(), nullptr, is_active))
                        canvas->SetActiveLayerIndex(layer_index);
                    if (ImGui::MenuItem(PHOS_TR("layer_manager.ctx_rename_ellipsis").c_str()))
                    {
                        rename_target_canvas_ = canvas;
                        rename_target_layer_index_ = layer_index;
                        const std::string current = canvas->GetLayerName(rename_target_layer_index_);
                        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", current.c_str());
                        rename_popup_serial_++;
                        rename_popup_active_serial_ = rename_popup_serial_;
                        rename_popup_requested_open_ = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(PHOS_TR("layer_manager.ctx_move_to_front").c_str()))
                        canvas->MoveLayer(layer_index, layer_count - 1);
                    if (ImGui::MenuItem(PHOS_TR("layer_manager.ctx_move_to_back").c_str()))
                        canvas->MoveLayer(layer_index, 0);
                    ImGui::Separator();
                    if (ImGui::MenuItem(PHOS_TR("layer_manager.ctx_remove_layer").c_str()))
                        canvas->RemoveLayer(layer_index);
                    ImGui::EndPopup();
                }

                // Drag source on the row.
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("PHOS_LAYER_INDEX", &layer_index, sizeof(int));
                    const std::string s = PHOS_TRF("layer_manager.drag_move_fmt", phos::i18n::Arg::Str(display_name));
                    ImGui::TextUnformatted(s.c_str());
                    ImGui::EndDragDropSource();
                }

                // Drop target: move src layer to this row's index.
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PHOS_LAYER_INDEX"))
                    {
                        if (payload->DataSize == sizeof(int))
                        {
                            const int src = *(const int*)payload->Data;
                            const int dst = layer_index;
                            if (src != dst)
                                canvas->MoveLayer(src, dst);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // Lay out row content over the selectable.
                const ImVec2 row_min = ImGui::GetItemRectMin();
                const ImVec2 row_max = ImGui::GetItemRectMax();
                const float pad_x = 8.0f;

                // Thumbnail (left)
                ImGui::SetCursorScreenPos(ImVec2(row_min.x + pad_x, row_min.y + pad_y));
                {
                    LayerThumbCache& tc = thumb_cache_[(size_t)layer_index];
                    const std::uint64_t rev = canvas->GetContentRevision();
                    const int cols = canvas->GetColumns();
                    const int rows = canvas->GetRows();
                    const int font_id = (int)canvas->GetFontId();
                    const bool bg_white = canvas->IsCanvasBackgroundWhite();

                    const bool params_match =
                        tc.valid &&
                        tc.cols == cols &&
                        tc.rows == rows &&
                        tc.font_id == font_id &&
                        tc.canvas_bg_white == bg_white;

                    // Refresh policy:
                    // - Always compute once if cache is missing/invalid.
                    // - Always refresh if fundamental render params changed (font/geometry/theme).
                    // - Otherwise, only refresh on explicit user interaction boundaries (mouse/key release).
                    const bool content_changed = (tc.canvas_revision != rev);
                    const bool must_refresh = !params_match;
                    const bool should_refresh_now = allow_thumbnail_refresh && content_changed;

                    if (!tc.valid || must_refresh || should_refresh_now)
                    {
                        ComputeLayerThumbnailGrid(*canvas, layer_index, tc.gw, tc.gh, tc.colors);
                        tc.cols = cols;
                        tc.rows = rows;
                        tc.font_id = font_id;
                        tc.canvas_bg_white = bg_white;
                        tc.canvas_revision = rev;
                        tc.valid = true;
                    }

                    DrawLayerThumbnailFromGrid(tc.colors, tc.gw, tc.gh, ImVec2(64.0f, thumb_h), !is_visible);
                }

                // Name (top line) + controls (second line) to match standard editors.
                const float x_after_thumb = row_min.x + pad_x + 64.0f + 10.0f;
                ImGui::SetCursorScreenPos(ImVec2(x_after_thumb, row_min.y + pad_y));

                const bool editing = (inline_rename_canvas_ == canvas && inline_rename_layer_index_ == layer_index);
                if (editing)
                {
                    ImGui::SetNextItemWidth(std::max(10.0f, row_max.x - x_after_thumb - pad_x));
                    if (inline_rename_request_focus_)
                    {
                        ImGui::SetKeyboardFocusHere();
                        inline_rename_request_focus_ = false;
                    }
                    const ImGuiInputTextFlags flags =
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll;
                    const bool enter = ImGui::InputText("##inline_rename", inline_rename_buf_, IM_ARRAYSIZE(inline_rename_buf_), flags);
                    const bool deactivate_commit = ImGui::IsItemDeactivatedAfterEdit();
                    if (enter || deactivate_commit)
                    {
                        canvas->SetLayerName(layer_index, std::string(inline_rename_buf_));
                        inline_rename_canvas_ = nullptr;
                        inline_rename_layer_index_ = -1;
                        inline_rename_buf_[0] = '\0';
                        inline_rename_request_focus_ = false;
                    }
                }
                else
                {
                    ImGui::TextUnformatted(display_name.c_str());
                    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                                    (is_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl)))
                    {
                        inline_rename_canvas_ = canvas;
                        inline_rename_layer_index_ = layer_index;
                        std::snprintf(inline_rename_buf_, sizeof(inline_rename_buf_), "%s", raw_name.c_str());
                        inline_rename_request_focus_ = true;
                    }
                }

                // Controls line 1 (below name): visibility + lock.
                const float y_controls_1 = row_min.y + pad_y + line_h + 2.0f;
                ImGui::SetCursorScreenPos(ImVec2(x_after_thumb, y_controls_1));

                bool vis = is_visible;
                if (ImGui::Checkbox("##vis", &vis))
                    canvas->SetLayerVisible(layer_index, vis);
                ImGui::SameLine();
                ImGui::TextUnformatted(PHOS_TR("layer_manager.visible").c_str());

                ImGui::SameLine();
                bool lock_transparency = is_locked;
                if (ImGui::Checkbox("##lock", &lock_transparency))
                    canvas->SetLayerTransparencyLocked(layer_index, lock_transparency);
                ImGui::SameLine();
                ImGui::TextUnformatted(PHOS_TR("layer_manager.lock_transparency").c_str());

                // Controls line 2: blend mode + opacity (widgets only).
                const float y_controls_2 = y_controls_1 + frame_h + 2.0f;
                ImGui::SetCursorScreenPos(ImVec2(x_after_thumb, y_controls_2));
                {
                    const phos::LayerBlendMode cur = canvas->GetLayerBlendMode(layer_index);
                    const char* preview = phos::LayerBlendModeToUiLabel(cur);
                    ImGui::SetNextItemWidth(170.0f);
                    if (ImGui::BeginCombo("##blend_mode", preview))
                    {
                        const phos::LayerBlendMode modes[] = {
                            phos::LayerBlendMode::Normal,
                            phos::LayerBlendMode::Multiply,
                            phos::LayerBlendMode::Screen,
                            phos::LayerBlendMode::Overlay,
                            phos::LayerBlendMode::Darken,
                            phos::LayerBlendMode::Lighten,
                            phos::LayerBlendMode::ColorDodge,
                            phos::LayerBlendMode::ColorBurn,
                        };
                        for (phos::LayerBlendMode m : modes)
                        {
                            const bool selected = (m == cur);
                            if (ImGui::Selectable(phos::LayerBlendModeToUiLabel(m), selected))
                                canvas->SetLayerBlendMode(layer_index, m);
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                // Blend opacity (0..100%). 100% = full blend effect, 0% = transparent contribution.
                ImGui::SameLine();
                {
                    const std::uint8_t a_u8 = canvas->GetLayerBlendAlpha(layer_index);
                    float pct = ((float)a_u8 * 100.0f) / 255.0f;
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("##blend_alpha", &pct, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
                    {
                        const int ai = (int)std::lround((double)pct * 255.0 / 100.0);
                        canvas->SetLayerBlendAlpha(layer_index, (std::uint8_t)std::clamp(ai, 0, 255));
                    }
                }

                // Restore cursor (see note above).
                ImGui::SetCursorScreenPos(cursor_after_row);
                // ImGui debug asserts if SetCursorPos/SetCursorScreenPos is used to extend a window
                // and no item is submitted afterwards. Submitting a zero-size dummy keeps ImGui happy
                // (especially in clipped lists with lots of items).
                ImGui::Dummy(ImVec2(0.0f, 0.0f));

                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}


