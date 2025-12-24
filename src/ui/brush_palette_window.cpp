#include "ui/brush_palette_window.h"

#include "imgui.h"

#include "ansl/ansl_native.h"
#include "core/paths.h"
#include "core/xterm256_palette.h"
#include "io/session/imgui_persistence.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/imgui_window_chrome.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

using nlohmann::json;

static inline AnsiCanvas::ColorIndex16 ClampIndex16(std::uint32_t v)
{
    if (v > (std::uint32_t)std::numeric_limits<AnsiCanvas::ColorIndex16>::max())
        return (AnsiCanvas::ColorIndex16)std::numeric_limits<AnsiCanvas::ColorIndex16>::max();
    return (AnsiCanvas::ColorIndex16)v;
}

static inline AnsiCanvas::ColorIndex16 LegacyColor32ToIndex16(std::uint32_t legacy_c32, AnsiCanvas* active_canvas)
{
    if (legacy_c32 == 0)
        return AnsiCanvas::kUnsetIndex16;

    if (active_canvas)
        return active_canvas->QuantizeColor32ToIndexPublic((AnsiCanvas::Color32)legacy_c32);

    // Fallback: quantize to xterm-256 based on RGB channels in IM_COL32 layout.
    const std::uint8_t r = (std::uint8_t)(legacy_c32 & 0xffu);
    const std::uint8_t g = (std::uint8_t)((legacy_c32 >> 8) & 0xffu);
    const std::uint8_t b = (std::uint8_t)((legacy_c32 >> 16) & 0xffu);
    const int idx = xterm256::NearestIndex(r, g, b);
    return (AnsiCanvas::ColorIndex16)idx;
}

static inline ImU32 Index16ToImU32(AnsiCanvas::ColorIndex16 idx, AnsiCanvas* active_canvas)
{
    if (idx == AnsiCanvas::kUnsetIndex16)
        return 0;
    if (active_canvas)
        return (ImU32)active_canvas->IndexToColor32Public(idx);
    // Fallback: interpret as xterm-256 index.
    const int i = (int)std::clamp((int)idx, 0, 255);
    return (ImU32)xterm256::Color32ForIndex(i);
}

static std::string DefaultBrushName(int idx)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Brush %d", idx);
    return std::string(buf);
}

static std::string EncodeUtf8(char32_t cp)
{
    return ansl::utf8::encode(cp);
}

static bool IsValidBrush(const AnsiCanvas::Brush& b)
{
    if (b.w <= 0 || b.h <= 0)
        return false;
    const size_t n = (size_t)b.w * (size_t)b.h;
    return b.cp.size() == n && b.fg.size() == n && b.bg.size() == n && b.attrs.size() == n;
}

BrushPaletteWindow::BrushPaletteWindow()
{
    file_path_ = PhosphorAssetPath("brush-palettes.json");
}

void BrushPaletteWindow::LoadFromSessionBrushPalette(SessionState* session, AnsiCanvas* active_canvas)
{
    if (!session)
        return;

    entries_.clear();
    inline_rename_index_ = -1;
    inline_rename_buf_[0] = '\0';
    inline_rename_request_focus_ = false;
    entries_.reserve(session->brush_palette.entries.size());
    for (const auto& se : session->brush_palette.entries)
    {
        if (se.w <= 0 || se.h <= 0)
            continue;
        const size_t n = (size_t)se.w * (size_t)se.h;
        if (se.cp.size() != n || se.fg.size() != n || se.bg.size() != n || se.attrs.size() != n)
            continue;

        AnsiCanvas::Brush b;
        b.w = se.w;
        b.h = se.h;
        b.cp.resize(n);
        b.fg.resize(n);
        b.bg.resize(n);
        b.attrs.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            b.cp[i] = (char32_t)se.cp[i];
            b.fg[i] = LegacyColor32ToIndex16(se.fg[i], active_canvas);
            b.bg[i] = LegacyColor32ToIndex16(se.bg[i], active_canvas);
            b.attrs[i] = (AnsiCanvas::Attrs)se.attrs[i];
        }
        if (!IsValidBrush(b))
            continue;

        Entry e;
        e.name = se.name;
        e.brush = std::move(b);
        entries_.push_back(std::move(e));
    }

    selected_ = session->brush_palette.selected;
    if (selected_ < -1) selected_ = -1;
    if (selected_ >= (int)entries_.size())
        selected_ = (int)entries_.size() - 1;
}

bool BrushPaletteWindow::LoadFromFile(const char* path, AnsiCanvas* active_canvas, std::string& error)
{
    error.clear();
    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }

    std::ifstream f(path);
    if (!f)
    {
        error = std::string("Failed to open ") + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!j.is_object())
    {
        error = "Expected JSON object in brush-palettes.json";
        return false;
    }

    if (!j.contains("brushes") || !j["brushes"].is_array())
    {
        error = "Expected 'brushes' array in brush-palettes.json";
        return false;
    }

    int schema_version = 1;
    if (j.contains("schema_version") && j["schema_version"].is_number_integer())
        schema_version = j["schema_version"].get<int>();

    int sel = -1;
    if (j.contains("selected") && j["selected"].is_number_integer())
        sel = j["selected"].get<int>();

    std::vector<Entry> parsed;
    for (const auto& item : j["brushes"])
    {
        if (!item.is_object())
            continue;

        Entry e;
        if (item.contains("name") && item["name"].is_string())
            e.name = item["name"].get<std::string>();

        int w = 0;
        int h = 0;
        if (item.contains("w") && item["w"].is_number_integer())
            w = item["w"].get<int>();
        if (item.contains("h") && item["h"].is_number_integer())
            h = item["h"].get<int>();
        if (w <= 0 || h <= 0)
            continue;

        const size_t n = (size_t)w * (size_t)h;

        auto read_u32_array = [&](const char* key, std::vector<std::uint32_t>& out) -> bool {
            out.clear();
            if (!item.contains(key) || !item[key].is_array())
                return false;
            out.reserve(n);
            for (const auto& v : item[key])
            {
                if (!v.is_number_unsigned() && !v.is_number_integer())
                    return false;
                out.push_back(v.get<std::uint32_t>());
            }
            return out.size() == n;
        };

        std::vector<std::uint32_t> cp;
        std::vector<std::uint32_t> fg;
        std::vector<std::uint32_t> bg;
        std::vector<std::uint32_t> attrs;
        if (!read_u32_array("cp", cp) ||
            !read_u32_array("fg", fg) ||
            !read_u32_array("bg", bg) ||
            !read_u32_array("attrs", attrs))
        {
            continue;
        }

        // Schema handling:
        // - v2+: fg/bg are palette indices (ColorIndex16).
        // - v1: historically fg/bg were packed Color32 (0 = unset). Some intermediate builds wrote
        //       indices while still labeling schema_version=1. We auto-detect per-brush.
        //
        // Detection rules (for v1):
        // - If we see an alpha byte (top 8 bits) or values > 0xffff, it's packed Color32.
        // - Else if max <= 255 and there exists a non-zero value, it's almost certainly indices.
        // - Else (notably the common "all zeros" case), treat as packed Color32 so 0 stays "unset"
        //   instead of becoming palette index 0 (black).
        bool fg_bg_are_indices = (schema_version >= 2);
        if (!fg_bg_are_indices)
        {
            const size_t scan_n = std::min(n, (size_t)64);
            bool saw_alpha_or_wide = false;
            bool saw_nonzero = false;
            std::uint32_t max_v = 0;
            for (size_t i = 0; i < scan_n; ++i)
            {
                const std::uint32_t fv = fg[i];
                const std::uint32_t bv = bg[i];
                max_v = std::max(max_v, std::max(fv, bv));
                saw_nonzero = saw_nonzero || (fv != 0u) || (bv != 0u);
                if (((fv & 0xff000000u) != 0u && fv != 0u) ||
                    ((bv & 0xff000000u) != 0u && bv != 0u) ||
                    fv > 0xffffu || bv > 0xffffu)
                {
                    saw_alpha_or_wide = true;
                    break;
                }
            }

            if (!saw_alpha_or_wide && max_v <= 255u && saw_nonzero)
                fg_bg_are_indices = true;
            else
                fg_bg_are_indices = false;
        }

        AnsiCanvas::Brush b;
        b.w = w;
        b.h = h;
        b.cp.resize(n);
        b.fg.resize(n);
        b.bg.resize(n);
        b.attrs.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            b.cp[i] = (char32_t)cp[i];
            if (fg_bg_are_indices)
            {
                b.fg[i] = ClampIndex16(fg[i]);
                b.bg[i] = ClampIndex16(bg[i]);
            }
            else
            {
                // Legacy schema: fg/bg stored as packed Color32 (0 = unset).
                b.fg[i] = LegacyColor32ToIndex16(fg[i], active_canvas);
                b.bg[i] = LegacyColor32ToIndex16(bg[i], active_canvas);
            }
            b.attrs[i] = (AnsiCanvas::Attrs)attrs[i];
        }
        if (!IsValidBrush(b))
            continue;

        e.brush = std::move(b);
        parsed.push_back(std::move(e));
    }

    entries_ = std::move(parsed);
    selected_ = std::clamp(sel, -1, (int)entries_.size() - 1);
    inline_rename_index_ = -1;
    inline_rename_buf_[0] = '\0';
    inline_rename_request_focus_ = false;
    return true;
}

bool BrushPaletteWindow::SaveToFile(const char* path, std::string& error) const
{
    error.clear();
    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }

    json j;
    // v2: fg/bg are stored as palette indices (ColorIndex16), not packed Color32.
    j["schema_version"] = 2;
    j["selected"] = selected_;

    json brushes = json::array();
    for (const auto& e : entries_)
    {
        const AnsiCanvas::Brush& b = e.brush;
        if (!IsValidBrush(b))
            continue;
        const size_t n = (size_t)b.w * (size_t)b.h;

        json item;
        item["name"] = e.name;
        item["w"] = b.w;
        item["h"] = b.h;

        json cp = json::array();
        json fg = json::array();
        json bg = json::array();
        json attrs = json::array();
        for (size_t i = 0; i < n; ++i)
        {
            cp.push_back((std::uint32_t)b.cp[i]);
            fg.push_back((std::uint32_t)b.fg[i]);
            bg.push_back((std::uint32_t)b.bg[i]);
            attrs.push_back((std::uint32_t)b.attrs[i]);
        }
        item["cp"] = std::move(cp);
        item["fg"] = std::move(fg);
        item["bg"] = std::move(bg);
        item["attrs"] = std::move(attrs);
        brushes.push_back(std::move(item));
    }
    j["brushes"] = std::move(brushes);

    std::ofstream f(path);
    if (!f)
    {
        error = std::string("Failed to write ") + path;
        return false;
    }

    try
    {
        f << j.dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    return true;
}

void BrushPaletteWindow::EnsureLoaded(AnsiCanvas* active_canvas, SessionState* session)
{
    if (loaded_)
        return;

    std::string err;
    if (!LoadFromFile(file_path_.c_str(), active_canvas, err))
    {
        last_error_ = err;

        // Migration: import from legacy session.json data if present.
        if (session && !session->brush_palette.entries.empty() && !migrated_from_session_)
        {
            LoadFromSessionBrushPalette(session, active_canvas);
            migrated_from_session_ = true;
            std::string save_err;
            if (!SaveToFile(file_path_.c_str(), save_err))
                last_error_ = save_err;
            else
                last_error_.clear();
        }
    }
    else
    {
        last_error_.clear();
    }

    loaded_ = true;
}

void BrushPaletteWindow::RenderTopBar(AnsiCanvas* active_canvas, SessionState* session)
{
    (void)session;
    const bool can_capture = active_canvas && active_canvas->HasSelection();

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##brush_name", "Name (optional)", new_name_buf_, IM_ARRAYSIZE(new_name_buf_));
    ImGui::SameLine();

    ImGui::BeginDisabled(!can_capture);
    if (ImGui::Button("Add from Selection"))
    {
        AnsiCanvas::Brush b;
        bool ok = false;
        if (capture_composite_)
            ok = active_canvas->CaptureBrushFromSelectionComposite(b);
        else
            ok = active_canvas->CaptureBrushFromSelection(b, /*layer_index=*/-1);

        if (ok && IsValidBrush(b))
        {
            Entry e;
            e.name = (new_name_buf_[0] != '\0') ? std::string(new_name_buf_) : DefaultBrushName((int)entries_.size() + 1);
            e.brush = std::move(b);
            entries_.push_back(std::move(e));
            selected_ = (int)entries_.size() - 1;
            // Apply immediately so tools see ctx.brush without requiring an extra click.
            if (active_canvas)
                (void)active_canvas->SetCurrentBrush(entries_.back().brush);
            request_activate_brush_tool_ = true;
            request_save_ = true;
            // Clear the buffer after successful add
            new_name_buf_[0] = '\0';
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool can_delete = (selected_ >= 0 && selected_ < (int)entries_.size());
    ImGui::BeginDisabled(!can_delete);
    if (ImGui::Button("Delete"))
    {
        const int erased_index = selected_;
        entries_.erase(entries_.begin() + erased_index);
        if (selected_ >= (int)entries_.size())
            selected_ = (int)entries_.size() - 1;
        // Keep inline-rename state consistent with the new indices.
        if (inline_rename_index_ == erased_index)
            inline_rename_index_ = -1;
        else if (inline_rename_index_ > erased_index)
            inline_rename_index_--;
        if (inline_rename_index_ < 0 || inline_rename_index_ >= (int)entries_.size())
            inline_rename_index_ = -1;
        if (inline_rename_index_ == -1)
        {
            inline_rename_buf_[0] = '\0';
            inline_rename_request_focus_ = false;
        }
        // Keep the active canvas brush synchronized with the current selection.
        if (active_canvas)
        {
            if (selected_ >= 0 && selected_ < (int)entries_.size() && IsValidBrush(entries_[(size_t)selected_].brush))
                (void)active_canvas->SetCurrentBrush(entries_[(size_t)selected_].brush);
            else
                active_canvas->ClearCurrentBrush();
        }
        request_save_ = true;
    }
    ImGui::EndDisabled();

    if (!can_capture)
    {
        ImGui::TextDisabled("Select a region on the canvas to capture a brush.");
    }
}

void BrushPaletteWindow::RenderSettingsContents()
{
    // File
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##brushpal_file", &file_path_);
    if (!last_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());
    if (ImGui::Button("Reload"))
        request_reload_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        request_save_ = true;

    ImGui::Separator();

    ImGui::Checkbox("Composite", &capture_composite_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderInt("Thumb", &thumb_px_, 32, 160, "%dpx");

    ImGui::Separator();
}

void BrushPaletteWindow::RenderGrid(AnsiCanvas* active_canvas, SessionState* session)
{
    (void)session;
    if (entries_.empty())
    {
        ImGui::TextDisabled("(No brushes yet)");
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float item_w = (float)thumb_px_ + style.FramePadding.x * 2.0f;
    int cols = (item_w > 0.0f) ? (int)std::floor((avail + style.ItemSpacing.x) / (item_w + style.ItemSpacing.x)) : 1;
    if (cols < 1) cols = 1;

    for (int i = 0; i < (int)entries_.size(); ++i)
    {
        ImGui::PushID(i);
        if (i % cols != 0)
            ImGui::SameLine();

        Entry& e = entries_[(size_t)i];
        const AnsiCanvas::Brush& b = e.brush;
        const bool valid = IsValidBrush(b);

        // Thumbnail button
        const ImVec2 button_sz((float)thumb_px_, (float)thumb_px_);
        const bool is_selected = (i == selected_);

        ImGui::BeginGroup();
        if (ImGui::Selectable("##sel", is_selected, ImGuiSelectableFlags_None, button_sz))
        {
            selected_ = i;
            if (active_canvas && valid)
                (void)active_canvas->SetCurrentBrush(b);
            request_activate_brush_tool_ = true;
            request_save_ = true;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();

        // Frame
        const ImU32 frame_col = ImGui::GetColorU32(is_selected ? ImGuiCol_ButtonActive : ImGuiCol_Border);
        dl->AddRect(rmin, rmax, frame_col, 2.0f);

        if (valid)
        {
            const float inner_pad = 4.0f;
            const ImVec2 inner_min(rmin.x + inner_pad, rmin.y + inner_pad);
            const ImVec2 inner_max(rmax.x - inner_pad, rmax.y - inner_pad);
            const float iw = std::max(1.0f, inner_max.x - inner_min.x);
            const float ih = std::max(1.0f, inner_max.y - inner_min.y);

            // Compute cell size (fit brush into thumbnail while preserving aspect).
            const float cell = std::max(1.0f, std::floor(std::min(iw / (float)b.w, ih / (float)b.h)));
            const float draw_w = cell * (float)b.w;
            const float draw_h = cell * (float)b.h;
            const ImVec2 origin(inner_min.x + (iw - draw_w) * 0.5f,
                                inner_min.y + (ih - draw_h) * 0.5f);

            const ImU32 default_fg = ImGui::GetColorU32(ImGuiCol_Text);

            for (int y = 0; y < b.h; ++y)
                for (int x = 0; x < b.w; ++x)
                {
                    const size_t idx = (size_t)y * (size_t)b.w + (size_t)x;
                    const ImVec2 cmin(origin.x + cell * (float)x, origin.y + cell * (float)y);
                    const ImVec2 cmax(cmin.x + cell, cmin.y + cell);

                    const AnsiCanvas::ColorIndex16 bg_idx = b.bg[idx];
                    const ImU32 bg_col = Index16ToImU32(bg_idx, active_canvas);
                    if (bg_idx != AnsiCanvas::kUnsetIndex16)
                        dl->AddRectFilled(cmin, cmax, bg_col);

                    const char32_t cp = b.cp[idx];
                    if (cp == U' ' && bg_idx == AnsiCanvas::kUnsetIndex16)
                        continue;

                    const AnsiCanvas::ColorIndex16 fg_idx = b.fg[idx];
                    const ImU32 fg = (fg_idx != AnsiCanvas::kUnsetIndex16) ? Index16ToImU32(fg_idx, active_canvas) : default_fg;
                    const std::string ch = EncodeUtf8(cp);

                    // Center glyph in the cell.
                    const ImVec2 ts = ImGui::CalcTextSize(ch.c_str(), ch.c_str() + ch.size());
                    const ImVec2 tp(cmin.x + (cell - ts.x) * 0.5f,
                                    cmin.y + (cell - ts.y) * 0.5f);
                    dl->AddText(tp, fg, ch.c_str(), ch.c_str() + ch.size());
                }
        }

        // Label
        const char* display_name = e.name.empty() ? "(unnamed)" : e.name.c_str();
        const float label_w = (float)thumb_px_;

        // Detect double-click on the label region *before* emitting any widgets,
        // so we can swap in InputText this same frame (focus + select-all works immediately).
        const ImVec2 label_pos = ImGui::GetCursorScreenPos();
        const ImVec2 label_ts = ImGui::CalcTextSize(display_name, nullptr, false, label_w);
        const float label_h = std::max(ImGui::GetTextLineHeight(), label_ts.y);
        const ImVec2 label_max(label_pos.x + label_w, label_pos.y + label_h);
        const bool hovered = ImGui::IsMouseHoveringRect(label_pos, label_max, /*clip=*/true);
        const bool start_edit = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (start_edit)
        {
            inline_rename_index_ = i;
            std::snprintf(inline_rename_buf_, sizeof(inline_rename_buf_), "%s", e.name.c_str());
            inline_rename_request_focus_ = true;
        }

        const bool editing = (inline_rename_index_ == i);
        ImGui::SetNextItemWidth(label_w);
        if (editing)
        {
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
                e.name = std::string(inline_rename_buf_);
                inline_rename_index_ = -1;
                inline_rename_buf_[0] = '\0';
                inline_rename_request_focus_ = false;
                request_save_ = true;
            }
        }
        else
        {
            ImGui::PushTextWrapPos(label_pos.x + label_w);
            ImGui::TextUnformatted(display_name);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();

        ImGui::PopID();
    }
}

bool BrushPaletteWindow::Render(const char* window_title,
                                bool* p_open,
                                AnsiCanvas* active_canvas,
                                SessionState* session,
                                bool apply_placement_this_frame)
{
    if (!window_title)
        window_title = "Brush Palette";

    if (session)
        ApplyImGuiWindowPlacement(*session, window_title, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None | (session ? GetImGuiWindowChromeExtraFlags(*session, window_title) : 0);
    const bool alpha_pushed = session ? PushImGuiWindowChromeAlpha(session, window_title) : false;

    const bool open = ImGui::Begin(window_title, p_open, flags);
    if (session)
    {
        CaptureImGuiWindowPlacement(*session, window_title);
        ApplyImGuiWindowChromeZOrder(session, window_title);
        RenderImGuiWindowChromeMenu(session, window_title);
    }

    // Title-bar ⋮ settings popup (in addition to the in-window collapsing header).
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##brushpal_kebab", "\xE2\x8B\xAE", has_close, has_collapse, &kebab_min, &kebab_max))
            ImGui::OpenPopup("##brushpal_settings");

        if (ImGui::IsPopupOpen("##brushpal_settings"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(520.0f, 420.0f));
        if (ImGui::BeginPopup("##brushpal_settings"))
        {
            ImGui::TextUnformatted("Settings");
            ImGui::Separator();
            RenderSettingsContents();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    if (!open)
    {
        ImGui::End();
        if (session) PopImGuiWindowChromeAlpha(alpha_pushed);
        return p_open ? *p_open : false;
    }

    EnsureLoaded(active_canvas, session);

    // Handle queued file operations (triggered by UI buttons).
    if (request_reload_)
    {
        request_reload_ = false;
        std::string err;
        if (!LoadFromFile(file_path_.c_str(), active_canvas, err))
            last_error_ = err;
        else
            last_error_.clear();
    }
    if (request_save_)
    {
        request_save_ = false;
        std::string err;
        if (!SaveToFile(file_path_.c_str(), err))
            last_error_ = err;
        else
            last_error_.clear();
    }

    // If the active canvas changed, re-apply the currently selected brush so tools stay in sync.
    if (active_canvas != last_active_canvas_)
    {
        last_active_canvas_ = active_canvas;
        if (active_canvas)
        {
            if (selected_ >= 0 && selected_ < (int)entries_.size() && IsValidBrush(entries_[(size_t)selected_].brush))
                (void)active_canvas->SetCurrentBrush(entries_[(size_t)selected_].brush);
            else
                active_canvas->ClearCurrentBrush();
        }
    }

    RenderTopBar(active_canvas, session);
    ImGui::Separator();
    RenderGrid(active_canvas, session);

    ImGui::End();
    if (session) PopImGuiWindowChromeAlpha(alpha_pushed);
    return p_open ? *p_open : true;
}


