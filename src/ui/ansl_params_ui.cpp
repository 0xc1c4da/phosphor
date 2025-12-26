#include "ui/ansl_params_ui.h"

#include "imgui.h"

#include "core/i18n.h"
#include "fonts/textmode_font_registry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool StrIContains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    const std::string h = ToLower(haystack);
    const std::string n = ToLower(needle);
    return h.find(n) != std::string::npos;
}

static bool ToggleButton(const char* label, bool v, const ImVec2& size = ImVec2(0, 0))
{
    ImVec4 col = ImGui::GetStyleColorVec4(v ? ImGuiCol_ButtonActive : ImGuiCol_Button);
    ImVec4 hov = ImGui::GetStyleColorVec4(v ? ImGuiCol_ButtonHovered : ImGuiCol_ButtonHovered);
    ImVec4 act = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

static bool RenderEnumSegmented(const char* label,
                               const AnslParamSpec& spec,
                               const std::string& cur,
                               int& out_idx)
{
    if (spec.enum_items.empty())
        return false;

    if (label && *label)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
    }

    bool changed = false;
    out_idx = 0;
    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
    {
        if (spec.enum_items[(size_t)i] == cur)
            out_idx = i;
    }

    ImGui::BeginGroup();
    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
    {
        if (i != 0)
            ImGui::SameLine();
        const bool selected = (i == out_idx);
        ImGui::PushID(i);
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(spec.enum_items[(size_t)i].c_str()))
        {
            out_idx = i;
            changed = true;
        }
        if (selected)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndGroup();
    return changed;
}

namespace
{
static int EncodeUtf8Local(char32_t cp, char out[5])
{
    // Minimal UTF-8 encoder. Invalid values become U+FFFD.
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        cp = 0xFFFDu;

    if (cp <= 0x7Fu)
    {
        out[0] = (char)cp;
        out[1] = 0;
        return 1;
    }
    if (cp <= 0x7FFu)
    {
        out[0] = (char)(0xC0u | ((cp >> 6) & 0x1Fu));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        out[2] = 0;
        return 2;
    }
    if (cp <= 0xFFFFu)
    {
        out[0] = (char)(0xE0u | ((cp >> 12) & 0x0Fu));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        out[3] = 0;
        return 3;
    }
    out[0] = (char)(0xF0u | ((cp >> 18) & 0x07u));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    out[4] = 0;
    return 4;
}

static bool IsBlankCellLocal(char32_t cp)
{
    return cp == U'\0' || cp == U' ' || cp == U'\u00A0';
}

struct FontPreviewCached
{
    bool ready = false;
    bool failed = false;
    std::string key; // cache key (id + opts + text)
    std::string id;
    std::string text;
    textmode_font::Bitmap bmp;
    std::string last_error;
    double last_used_time = 0.0;
};

static std::string FontPreviewKey(const std::string& id,
                                 const std::string& text,
                                 const textmode_font::RenderOptions& opts)
{
    // Keep simple/stable; opts fields are small integers/bools.
    return id + "|" + text + "|" + std::to_string((int)opts.mode) + "|" + std::to_string(opts.outline_style) + "|" +
           (opts.use_font_colors ? "1" : "0") + "|" + (opts.icecolors ? "1" : "0");
}

static std::string ShortenForPreview(std::string s, int max_len)
{
    if ((int)s.size() <= max_len)
        return s;
    s.resize((size_t)max_len);
    return s;
}

static bool RenderFontPreviewBitmap(textmode_font::Registry* reg,
                                   const std::string& font_id,
                                   const std::string& preferred_text,
                                   const textmode_font::RenderOptions& opts,
                                   textmode_font::Bitmap& out_bmp,
                                   std::string& out_err)
{
    out_bmp = textmode_font::Bitmap{};
    out_err.clear();
    if (!reg)
    {
        out_err = "font registry not available";
        return false;
    }

    // Safety: avoid rendering pathological sizes in the UI.
    constexpr int kMaxW = 240;
    constexpr int kMaxH = 80;
    constexpr int kMaxCells = 12000;

    std::vector<std::string> candidates;
    candidates.reserve(6);
    if (!preferred_text.empty())
    {
        candidates.push_back(preferred_text);
        candidates.push_back(ShortenForPreview(preferred_text, 12));
        candidates.push_back(ShortenForPreview(preferred_text, 8));
        candidates.push_back(ShortenForPreview(preferred_text, 4));
    }
    candidates.push_back("PHOSPHOR");
    candidates.push_back("Hi");

    // De-dup candidates (small N).
    std::vector<std::string> uniq;
    uniq.reserve(candidates.size());
    for (auto& c : candidates)
    {
        if (c.empty())
            continue;
        bool exists = false;
        for (const auto& u : uniq)
            exists = exists || (u == c);
        if (!exists)
            uniq.push_back(std::move(c));
    }

    for (const auto& text : uniq)
    {
        textmode_font::Bitmap bmp;
        std::string err;
        if (!reg->Render(font_id, text, opts, bmp, err))
        {
            if (!err.empty())
                out_err = err;
            continue;
        }
        const int w = bmp.w;
        const int h = bmp.h;
        if (w <= 0 || h <= 0)
        {
            out_err = "render produced empty dimensions";
            continue;
        }
        const int cells = w * h;
        if (w > kMaxW || h > kMaxH || cells > kMaxCells)
        {
            out_err = "preview too large (" + std::to_string(w) + "x" + std::to_string(h) + ")";
            continue;
        }
        if (bmp.cp.size() != (size_t)cells)
        {
            out_err = "cp size mismatch";
            continue;
        }
        out_bmp = std::move(bmp);
        return true;
    }
    if (out_err.empty())
        out_err = "render failed for all sample strings";
    return false;
}

static void DrawBitmapThumbnail(ImDrawList* dl,
                                const ImVec2& p0,
                                const ImVec2& p1,
                                const textmode_font::Bitmap& bmp,
                                int max_cols,
                                int max_rows)
{
    if (!dl)
        return;
    if (bmp.w <= 0 || bmp.h <= 0 || bmp.cp.empty())
        return;

    ImFont* font = ImGui::GetFont();
    const float base_font_sz = ImGui::GetFontSize();
    const float base_cell_w = ImGui::CalcTextSize("M").x;
    const float base_cell_h = ImGui::GetTextLineHeight();
    if (!font || base_font_sz <= 0.0f || base_cell_w <= 0.0f || base_cell_h <= 0.0f)
        return;

    const int cols = std::max(0, std::min(max_cols, bmp.w));
    const int rows = std::max(0, std::min(max_rows, bmp.h));
    if (cols <= 0 || rows <= 0)
        return;

    const ImU32 col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 col_border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 col_text_fallback = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddRectFilled(p0, p1, col_bg, 3.0f);
    dl->AddRect(p0, p1, col_border, 3.0f);

    const ImVec2 inner0(p0.x + 4.0f, p0.y + 4.0f);
    const ImVec2 inner1(p1.x - 4.0f, p1.y - 4.0f);
    dl->PushClipRect(inner0, inner1, true);

    const float max_w_px = inner1.x - inner0.x;
    const float max_h_px = inner1.y - inner0.y;
    if (max_w_px <= 1.0f || max_h_px <= 1.0f)
    {
        dl->PopClipRect();
        return;
    }

    // Shrink-to-fit (never upscale) so we can show the whole preview bitmap.
    const float scale_x = max_w_px / ((float)cols * base_cell_w);
    const float scale_y = max_h_px / ((float)rows * base_cell_h);
    const float scale = std::min(1.0f, std::max(0.0f, std::min(scale_x, scale_y)));
    const float cell_w = base_cell_w * scale;
    const float cell_h = base_cell_h * scale;
    const float font_sz = base_font_sz * scale;

    const float content_w = (float)cols * cell_w;
    const float content_h = (float)rows * cell_h;
    const float ox = inner0.x + std::max(0.0f, (max_w_px - content_w) * 0.5f);
    const float oy = inner0.y + std::max(0.0f, (max_h_px - content_h) * 0.5f);

    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            const int idx = y * bmp.w + x;
            if (idx < 0 || (size_t)idx >= bmp.cp.size())
                continue;

            char32_t cp = bmp.cp[(size_t)idx];
            if (IsBlankCellLocal(cp))
                cp = U' ';

            const ImU32 fg = (idx < (int)bmp.fg.size() && bmp.fg[(size_t)idx] != 0u) ? (ImU32)bmp.fg[(size_t)idx] : col_text_fallback;
            const ImU32 bg = (idx < (int)bmp.bg.size()) ? (ImU32)bmp.bg[(size_t)idx] : 0u;

            const ImVec2 cell0(ox + (float)x * cell_w, oy + (float)y * cell_h);
            const ImVec2 cell1(cell0.x + cell_w, cell0.y + cell_h);
            if (bg != 0u)
                dl->AddRectFilled(cell0, cell1, bg);

            // Skip drawing spaces to reduce draw calls a bit.
            if (cp != U' ')
            {
                char buf[5] = {0, 0, 0, 0, 0};
                (void)EncodeUtf8Local(cp, buf);
                dl->AddText(font, font_sz, cell0, fg, buf);
            }
        }
    }
    dl->PopClipRect();
}

static bool RenderFontEnumComboWithPreviews(const char* label,
                                            const AnslParamSpec& spec,
                                            AnslScriptEngine& engine,
                                            const std::string& cur_value,
                                            std::string& inout_filter)
{
    // Returns true if selection changed.
    const textmode_font::Registry* reg_c = engine.GetFontRegistry();
    textmode_font::Registry* reg = const_cast<textmode_font::Registry*>(reg_c);

    auto display_name_for_value = [&](const std::string& v) -> std::string {
        if (v.empty())
            return "(none)";
        if (v == "(no fonts)")
            return "(no fonts)";
        if (reg)
        {
            if (const auto* e = reg->Find(v))
            {
                if (!e->meta.name.empty())
                    return e->meta.name;
                return e->label;
            }
        }
        return v;
    };

    auto kind_suffix_for_value = [&](const std::string& v) -> const char* {
        if (v.empty() || v == "(no fonts)")
            return "";
        if (reg)
        {
            if (const auto* e = reg->Find(v))
                return (e->meta.kind == textmode_font::Kind::Tdf) ? " (TDF)" : " (Figlet)";
        }
        // Match the UX request: show (Figlet) / (TDF)
        return "";
    };

    const std::string preview_label = display_name_for_value(cur_value);
    if (!ImGui::BeginCombo(label, preview_label.c_str(), ImGuiComboFlags_HeightLarge))
        return false;

    // Filter input (make it obvious + auto-focus).
    ImGui::AlignTextToFramePadding();
    {
        const std::string label = PHOS_TR("common.filter_colon") + ":";
        ImGui::TextUnformatted(label.c_str());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    char buf[256] = {};
    if (!inout_filter.empty())
        std::snprintf(buf, sizeof(buf), "%s", inout_filter.c_str());
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##filter", "type to filter…", buf, sizeof(buf)))
        inout_filter = buf;
    ImGui::Separator();

    // Pull current render opts from other params (best-effort).
    textmode_font::RenderOptions ro;
    {
        bool edit = false;
        int outline = 0;
        (void)engine.GetParamBool("editMode", edit);
        (void)engine.GetParamInt("outlineStyle", outline);
        ro.mode = edit ? textmode_font::RenderMode::Edit : textmode_font::RenderMode::Display;
        ro.outline_style = std::clamp(outline, 0, 64);
        // Always render previews with intrinsic font colors enabled. The tool itself can
        // still choose whether to stamp colors; the picker should help users see what a
        // font looks like.
        ro.use_font_colors = true;
        ro.icecolors = true;
    }

    // Preview cache: render cost is in Registry::Render, so cache the resulting Bitmap.
    static std::unordered_map<std::string, FontPreviewCached> s_cache;
    static int s_cache_frame = -1;
    static int s_budget = 0;
    const int frame = ImGui::GetFrameCount();
    if (frame != s_cache_frame)
    {
        s_cache_frame = frame;
        s_budget = 2; // max new font renders per frame while popup is open
    }

    auto touch = [&](FontPreviewCached& e) {
        e.last_used_time = ImGui::GetTime();
    };
    auto evict_if_needed = [&]() {
        constexpr size_t kMaxEntries = 160;
        while (s_cache.size() > kMaxEntries)
        {
            auto it_min = s_cache.end();
            double best_t = 1e300;
            for (auto it = s_cache.begin(); it != s_cache.end(); ++it)
            {
                if (it->second.last_used_time < best_t)
                {
                    best_t = it->second.last_used_time;
                    it_min = it;
                }
            }
            if (it_min == s_cache.end())
                break;
            s_cache.erase(it_min);
        }
    };

    // Filter + clip list.
    std::vector<int> filtered;
    filtered.reserve(spec.enum_items.size());
    for (int i = 0; i < (int)spec.enum_items.size(); ++i)
    {
        const std::string& v = spec.enum_items[(size_t)i];
        if (!inout_filter.empty())
        {
            const std::string dn = display_name_for_value(v);
            if (!StrIContains(dn, inout_filter) && !StrIContains(v, inout_filter))
                continue;
        }
        filtered.push_back(i);
    }

    bool changed = false;
    ImGui::BeginChild("##font_combo_list", ImVec2(0.0f, 420.0f), false);
    ImGuiListClipper clipper;
    clipper.Begin((int)filtered.size());
    while (clipper.Step())
    {
        for (int li = clipper.DisplayStart; li < clipper.DisplayEnd; ++li)
        {
            const int i = filtered[(size_t)li];
            const std::string& v = spec.enum_items[(size_t)i];
            const bool is_sel = (v == cur_value);

            const std::string disp = display_name_for_value(v);

            // Layout: a big selectable row; we draw preview + text ourselves.
            ImGui::PushID(i);
            const float row_h = 120.0f;
            if (ImGui::Selectable("##font_item", is_sel, 0, ImVec2(0.0f, row_h)))
            {
                engine.SetParamEnum(spec.key, v);
                changed = true;
            }
            const ImVec2 r0 = ImGui::GetItemRectMin();
            const ImVec2 r1 = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Preview box
            const ImVec2 pv0(r0.x + 8.0f, r0.y + 8.0f);
            const ImVec2 pv1(r1.x - 8.0f, r1.y - 8.0f);

            // Render/cached preview bitmap (skip sentinel).
            if (v != "(no fonts)" && reg)
            {
                const auto* e = reg->Find(v);
                const std::string preferred_text = (e && !e->meta.name.empty()) ? e->meta.name : disp;

                const std::string k = FontPreviewKey(v, preferred_text, ro);
                auto itc = s_cache.find(k);
                if (itc == s_cache.end())
                {
                    FontPreviewCached c;
                    c.key = k;
                    c.id = v;
                    c.text = preferred_text;
                    c.last_used_time = ImGui::GetTime();
                    itc = s_cache.emplace(k, std::move(c)).first;
                }
                FontPreviewCached& c = itc->second;
                touch(c);

                if (!c.ready && !c.failed && s_budget > 0)
                {
                    textmode_font::Bitmap bmp;
                    std::string perr;
                    if (RenderFontPreviewBitmap(reg, v, preferred_text, ro, bmp, perr))
                    {
                        c.bmp = std::move(bmp);
                        c.ready = true;
                    }
                    else
                    {
                        c.failed = true;
                        c.last_error = perr;
                    }
                    s_budget--;
                    evict_if_needed();
                }

                if (c.ready)
                {
                    DrawBitmapThumbnail(dl, pv0, pv1, c.bmp, /*max_cols=*/64, /*max_rows=*/18);
                }
                else
                {
                    const ImU32 col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
                    const ImU32 col_border = ImGui::GetColorU32(ImGuiCol_Border);
                    dl->AddRectFilled(pv0, pv1, col_bg, 3.0f);
                    dl->AddRect(pv0, pv1, col_border, 3.0f);
                    const char* msg = c.failed ? "(preview unavailable)" : "(rendering preview…)";
                    const ImVec2 tpos(pv0.x + 10.0f, pv0.y + 10.0f);
                    dl->AddText(tpos, ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
                    if (c.failed && !c.last_error.empty())
                    {
                        // Tooltip with error details to help debug fonts/ids.
                        const ImVec2 mp = ImGui::GetMousePos();
                        const bool hover = (mp.x >= pv0.x && mp.x <= pv1.x && mp.y >= pv0.y && mp.y <= pv1.y);
                        if (hover)
                            ImGui::SetTooltip("%s", c.last_error.c_str());
                    }
                }
            }
            else
            {
                const ImU32 col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
                const ImU32 col_border = ImGui::GetColorU32(ImGuiCol_Border);
                dl->AddRectFilled(pv0, pv1, col_bg, 3.0f);
                dl->AddRect(pv0, pv1, col_border, 3.0f);
                const ImVec2 tpos(pv0.x + 10.0f, pv0.y + 10.0f);
                dl->AddText(tpos, ImGui::GetColorU32(ImGuiCol_TextDisabled), "(no fonts)");
            }

            // Bottom-right overlay label: "<name> (Figlet/TDF)".
            if (v != "(no fonts)")
            {
                std::string overlay = disp;
                overlay += kind_suffix_for_value(v);

                const ImVec2 ts = ImGui::CalcTextSize(overlay.c_str());
                const float pad_x = 6.0f;
                const float pad_y = 3.0f;
                const ImVec2 box1(pv1.x - 6.0f, pv1.y - 6.0f);
                ImVec2 box0(box1.x - ts.x - pad_x * 2.0f, box1.y - ts.y - pad_y * 2.0f);
                // Keep the label box inside the preview tile; text is clipped if needed.
                box0.x = std::max(box0.x, pv0.x + 4.0f);
                box0.y = std::max(box0.y, pv0.y + 4.0f);

                dl->PushClipRect(pv0, pv1, true);
                const ImU32 bg = IM_COL32(0, 0, 0, 170);
                dl->AddRectFilled(box0, box1, bg, 4.0f);
                dl->AddText(ImVec2(box0.x + pad_x, box0.y + pad_y), ImGui::GetColorU32(ImGuiCol_Text), overlay.c_str());
                dl->PopClipRect();
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::EndCombo();
    return changed;
}
} // namespace

static bool RenderParamControl(const AnslParamSpec& spec, AnslScriptEngine& engine, bool compact)
{
    // In compact mode, enforce consistent labels for common cross-tool toggles.
    // Many tools use verbose labels ("Fallback: Use FG") which makes the compact row inconsistent.
    const bool is_use_fg = (spec.key == "useFg");
    const bool is_use_bg = (spec.key == "useBg");
    const bool is_colour_source = (spec.key == "fgSource") || (spec.key == "bgSource");
    const char* label =
        (compact && is_use_fg) ? "FG" :
        (compact && is_use_bg) ? "BG" :
        (compact && is_colour_source && spec.label == "Source") ? "Src" :
        (spec.label.empty() ? spec.key.c_str() : spec.label.c_str());
    const std::string ui = ToLower(spec.ui);
    bool changed = false;

    // Optional enablement condition (bool param gate).
    bool enabled = true;
    if (!spec.enabled_if.empty())
    {
        bool gate = false;
        if (engine.GetParamBool(spec.enabled_if, gate))
            enabled = gate;
    }
    if (!enabled)
        ImGui::BeginDisabled(true);

    if (spec.width > 0.0f)
        ImGui::SetNextItemWidth(spec.width);

    switch (spec.type)
    {
        case AnslParamType::Bool:
        {
            bool v = false;
            if (!engine.GetParamBool(spec.key, v))
                return false;

            const bool want_toggle = (ui == "toggle") || (compact && ui != "checkbox");
            if (want_toggle)
            {
                if (ToggleButton(label, v))
                {
                    engine.SetParamBool(spec.key, !v);
                    changed = true;
                }
            }
            else
            {
                if (ImGui::Checkbox(label, &v))
                {
                    engine.SetParamBool(spec.key, v);
                    changed = true;
                }
            }
            break;
        }
        case AnslParamType::Button:
        {
            // Buttons are actions: render as normal buttons, but allow compact mode styling.
            if (compact)
            {
                if (ImGui::SmallButton(label))
                {
                    (void)engine.FireParamButton(spec.key);
                    changed = true;
                }
            }
            else if (ImGui::Button(label))
            {
                (void)engine.FireParamButton(spec.key);
                changed = true;
            }
            break;
        }
        case AnslParamType::Int:
        {
            int v = 0;
            if (!engine.GetParamInt(spec.key, v))
                return false;

            const bool has_range = (spec.int_min != spec.int_max);
            const bool want_slider = (ui == "slider") || (has_range && ui != "drag");
            if (compact)
            {
                // Compact: force label on the left for consistent tool bars (avoid SliderInt label-on-right).
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                if (spec.width > 0.0f)
                    ImGui::SetNextItemWidth(spec.width);
                else
                    ImGui::SetNextItemWidth(180.0f);

                int v2 = v;
                const char* wid = "##int";
                bool edited = false;
                if (has_range && want_slider)
                    edited = ImGui::SliderInt(wid, &v2, spec.int_min, spec.int_max);
                else
                    edited = ImGui::DragInt(wid, &v2, (float)std::max(1, spec.int_step));

                if (edited)
                {
                    // Quantize to step.
                    const int step = std::max(1, spec.int_step);
                    if (has_range && step > 1)
                        v2 = spec.int_min + ((v2 - spec.int_min) / step) * step;
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                if (has_range && want_slider)
                {
                    int v2 = v;
                    if (ImGui::SliderInt(label, &v2, spec.int_min, spec.int_max))
                    {
                        // Quantize to step.
                        const int step = std::max(1, spec.int_step);
                    if (step > 1)
                        v2 = spec.int_min + ((v2 - spec.int_min) / step) * step;
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                int v2 = v;
                if (ImGui::DragInt(label, &v2, (float)std::max(1, spec.int_step)))
                {
                    engine.SetParamInt(spec.key, v2);
                    changed = true;
                    }
                }
            }
            break;
        }
        case AnslParamType::Float:
        {
            float v = 0.0f;
            if (!engine.GetParamFloat(spec.key, v))
                return false;

            const bool has_range = (spec.float_min != spec.float_max);
            const bool want_slider = (ui == "slider") || (has_range && ui != "drag");
            if (compact)
            {
                // Compact: force label on the left for consistent tool bars (avoid SliderFloat label-on-right).
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                if (spec.width > 0.0f)
                    ImGui::SetNextItemWidth(spec.width);
                else
                    ImGui::SetNextItemWidth(180.0f);

                float v2 = v;
                const char* wid = "##float";
                bool edited = false;
                if (has_range && want_slider)
                    edited = ImGui::SliderFloat(wid, &v2, spec.float_min, spec.float_max);
                else
                    edited = ImGui::DragFloat(wid, &v2, spec.float_step);

                if (edited)
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                if (has_range && want_slider)
                {
                    float v2 = v;
                if (ImGui::SliderFloat(label, &v2, spec.float_min, spec.float_max))
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                }
            }
            else
            {
                float v2 = v;
                if (ImGui::DragFloat(label, &v2, spec.float_step))
                {
                    engine.SetParamFloat(spec.key, v2);
                    changed = true;
                    }
                }
            }
            break;
        }
        case AnslParamType::Enum:
        {
            std::string cur;
            if (!engine.GetParamEnum(spec.key, cur))
                return false;
            if (spec.enum_items.empty())
                return false;

            // Fonts have huge enums; always prefer the searchable combo there.
            const bool want_filter_combo = (ui == "combo_filter") || (spec.key == "font");

            const bool want_segmented =
                !want_filter_combo && ((ui == "segmented") || (compact && ui != "combo" && (int)spec.enum_items.size() <= 6));

            if (want_segmented)
            {
                int idx2 = 0;
                if (RenderEnumSegmented(label, spec, cur, idx2))
                {
                    idx2 = std::clamp(idx2, 0, (int)spec.enum_items.size() - 1);
                    engine.SetParamEnum(spec.key, spec.enum_items[(size_t)idx2]);
                    changed = true;
                }
            }
            else if (want_filter_combo)
            {
                // Combo with inline filter field (useful for large enums like fonts).
                // Keep filter state per-widget ID.
                static std::unordered_map<ImGuiID, std::string> s_enum_filters;
                ImGui::PushID("combo_filter");
                const ImGuiID fid = ImGui::GetID(spec.key.c_str());
                std::string& f = s_enum_filters[fid];

                if (spec.key == "font")
                {
                    changed = RenderFontEnumComboWithPreviews(label, spec, engine, cur, f) || changed;
                }
                else
                {
                    const char* preview = cur.c_str();
                    if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge))
                    {
                        // Filter input (make it obvious + auto-focus).
                        ImGui::AlignTextToFramePadding();
                        {
                            const std::string label = PHOS_TR("common.filter_colon") + ":";
                            ImGui::TextUnformatted(label.c_str());
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        char buf[256] = {};
                        if (!f.empty())
                            std::snprintf(buf, sizeof(buf), "%s", f.c_str());
                        if (ImGui::IsWindowAppearing())
                            ImGui::SetKeyboardFocusHere();
                        if (ImGui::InputTextWithHint("##filter", "type to filter…", buf, sizeof(buf)))
                            f = buf;
                        ImGui::Separator();

                        int picked_idx = -1;
                        for (int i = 0; i < (int)spec.enum_items.size(); ++i)
                        {
                            const std::string& item = spec.enum_items[(size_t)i];
                            if (!f.empty() && !StrIContains(item, f))
                                continue;
                            const bool is_sel = (item == cur);
                            if (ImGui::Selectable(item.c_str(), is_sel))
                                picked_idx = i;
                            if (is_sel)
                                ImGui::SetItemDefaultFocus();
                        }
                        if (picked_idx >= 0)
                        {
                            engine.SetParamEnum(spec.key, spec.enum_items[(size_t)picked_idx]);
                            changed = true;
                        }
                        ImGui::EndCombo();
                    }
                }
                ImGui::PopID();
            }
            else
            {
                int cur_idx = 0;
                for (int i = 0; i < (int)spec.enum_items.size(); ++i)
                {
                    if (spec.enum_items[(size_t)i] == cur)
                    {
                        cur_idx = i;
                        break;
                    }
                }

                // Build a stable char* array for ImGui::Combo.
                // Note: pointers reference strings owned by spec (stable for this frame).
                std::vector<const char*> items;
                items.reserve(spec.enum_items.size());
                for (const auto& s : spec.enum_items)
                    items.push_back(s.c_str());

                int idx2 = cur_idx;
                if (ImGui::Combo(label, &idx2, items.data(), (int)items.size()))
                {
                    idx2 = std::clamp(idx2, 0, (int)spec.enum_items.size() - 1);
                    engine.SetParamEnum(spec.key, spec.enum_items[(size_t)idx2]);
                    changed = true;
                }
            }
            break;
        }
    }

    if (!spec.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", spec.tooltip.c_str());

    if (!enabled)
        ImGui::EndDisabled();

    return changed;
}

static bool IsSkippedKey(const AnslParamSpec& s, const AnslParamsUISkipList* skip)
{
    if (!skip || !skip->keys || skip->count <= 0)
        return false;
    for (int i = 0; i < skip->count; ++i)
    {
        const char* k = skip->keys[i];
        if (!k || !*k)
            continue;
        if (s.key == k)
            return true;
    }
    return false;
}

bool RenderAnslParamByKey(const char* id, AnslScriptEngine& engine, const char* key, bool compact)
{
    if (!key || !*key)
        return false;
    if (!engine.HasParams())
        return false;

    const auto& specs = engine.GetParamSpecs();
    const AnslParamSpec* found = nullptr;
    for (const auto& s : specs)
    {
        if (s.key == key)
        {
            found = &s;
            break;
        }
    }
    if (!found)
        return false;

    ImGui::PushID(id ? id : "ansl_param_by_key");
    ImGui::PushID(key);
    const bool changed = RenderParamControl(*found, engine, compact);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUIPrimaryBar(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params_primary";

    ImGui::PushID(id);
    bool changed = false;

    if (!engine.HasParams())
    {
        ImGui::TextDisabled("%s", PHOS_TR("common.no_parameters").c_str());
        ImGui::PopID();
        return false;
    }

    const auto& specs = engine.GetParamSpecs();
    // Primary bar (compact): show "primary" params first.
    bool any_primary = false;
    for (const auto& s : specs)
        any_primary = any_primary || (s.primary && !IsSkippedKey(s, skip));

    if (any_primary)
    {
        bool have_prev_inline = false;
        for (const auto& s : specs)
        {
            if (!s.primary)
                continue;
            if (IsSkippedKey(s, skip))
                continue;

            if (have_prev_inline && s.inline_with_prev)
                ImGui::SameLine();
            ImGui::PushID(s.key.c_str());
            changed = RenderParamControl(s, engine, /*compact=*/true) || changed;
            ImGui::PopID();
            have_prev_inline = true;
        }
    }

    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUIAdvanced(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params_advanced";

    ImGui::PushID(id);
    bool changed = false;

    if (!engine.HasParams())
    {
        ImGui::TextDisabled("%s", PHOS_TR("common.no_parameters").c_str());
        ImGui::PopID();
        return false;
    }

    const auto& specs = engine.GetParamSpecs();
    bool any_advanced = false;
    for (const auto& s : specs)
        any_advanced = any_advanced || (!s.primary && !IsSkippedKey(s, skip));

    if (!any_advanced)
    {
        ImGui::PopID();
        return false;
    }

            std::string cur_section;
            bool section_open = false;
            bool have_prev_inline = false;
            for (const auto& s : specs)
            {
                if (s.primary)
                    continue;
        if (IsSkippedKey(s, skip))
            continue;

                const std::string sec = s.section.empty() ? "General" : s.section;
                if (sec != cur_section)
                {
                    cur_section = sec;
                    have_prev_inline = false;
                    section_open = ImGui::CollapsingHeader(cur_section.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!section_open)
                    continue;

                if (have_prev_inline && s.inline_with_prev)
                    ImGui::SameLine();

                ImGui::PushID(s.key.c_str());
                changed = RenderParamControl(s, engine, /*compact=*/false) || changed;
                ImGui::PopID();
                have_prev_inline = true;
            }

    ImGui::PopID();
    return changed;
}

bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip)
{
    if (!id)
        id = "ansl_params";

    ImGui::PushID(id);
    bool changed = false;

    changed = RenderAnslParamsUIPrimaryBar("##primary", engine, skip) || changed;
    if (engine.HasParams())
    {
        // Only add a separator if advanced exists.
        bool any_advanced = false;
        for (const auto& s : engine.GetParamSpecs())
            any_advanced = any_advanced || (!s.primary && !IsSkippedKey(s, skip));
        if (any_advanced)
        {
            ImGui::Separator();
            changed = RenderAnslParamsUIAdvanced("##advanced", engine, skip) || changed;
        }
    }

    ImGui::PopID();
    return changed;
}


