#include "ui/brush_palette_window.h"

#include "imgui.h"

#include "ansl/ansl_native.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"

#include <algorithm>
#include <cstdio>
#include <utility>

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

void BrushPaletteWindow::LoadFromSessionIfNeeded(SessionState* session)
{
    if (loaded_from_session_)
        return;
    loaded_from_session_ = true;
    if (!session)
        return;

    entries_.clear();
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
            b.fg[i] = (AnsiCanvas::Color32)se.fg[i];
            b.bg[i] = (AnsiCanvas::Color32)se.bg[i];
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

void BrushPaletteWindow::SaveToSession(SessionState* session) const
{
    if (!session)
        return;

    session->brush_palette.entries.clear();
    session->brush_palette.entries.reserve(entries_.size());
    for (const auto& e : entries_)
    {
        const AnsiCanvas::Brush& b = e.brush;
        if (!IsValidBrush(b))
            continue;
        const size_t n = (size_t)b.w * (size_t)b.h;
        SessionState::BrushPaletteEntry se;
        se.name = e.name;
        se.w = b.w;
        se.h = b.h;
        se.cp.resize(n);
        se.fg.resize(n);
        se.bg.resize(n);
        se.attrs.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            se.cp[i] = (std::uint32_t)b.cp[i];
            se.fg[i] = (std::uint32_t)b.fg[i];
            se.bg[i] = (std::uint32_t)b.bg[i];
            se.attrs[i] = (std::uint32_t)b.attrs[i];
        }
        session->brush_palette.entries.push_back(std::move(se));
    }
    session->brush_palette.selected = selected_;
}

void BrushPaletteWindow::RenderTopBar(AnsiCanvas* active_canvas, SessionState* session)
{
    const bool can_capture = active_canvas && active_canvas->HasSelection();

    ImGui::Checkbox("Composite", &capture_composite_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderInt("Thumb", &thumb_px_, 32, 160, "%dpx");

    ImGui::Separator();

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
            SaveToSession(session);
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
        entries_.erase(entries_.begin() + selected_);
        if (selected_ >= (int)entries_.size())
            selected_ = (int)entries_.size() - 1;
        // Keep the active canvas brush synchronized with the current selection.
        if (active_canvas)
        {
            if (selected_ >= 0 && selected_ < (int)entries_.size() && IsValidBrush(entries_[(size_t)selected_].brush))
                (void)active_canvas->SetCurrentBrush(entries_[(size_t)selected_].brush);
            else
                active_canvas->ClearCurrentBrush();
        }
        SaveToSession(session);
    }
    ImGui::EndDisabled();

    if (!can_capture)
    {
        ImGui::TextDisabled("Select a region on the canvas to capture a brush.");
    }
}

void BrushPaletteWindow::RenderGrid(AnsiCanvas* active_canvas, SessionState* session)
{
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

        const Entry& e = entries_[(size_t)i];
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
            SaveToSession(session);
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

                    const ImU32 bg = (ImU32)b.bg[idx];
                    if (bg != 0)
                        dl->AddRectFilled(cmin, cmax, bg);

                    const char32_t cp = b.cp[idx];
                    if (cp == U' ' && bg == 0)
                        continue;

                    const ImU32 fg = (b.fg[idx] != 0) ? (ImU32)b.fg[idx] : default_fg;
                    const std::string ch = EncodeUtf8(cp);

                    // Center glyph in the cell.
                    const ImVec2 ts = ImGui::CalcTextSize(ch.c_str(), ch.c_str() + ch.size());
                    const ImVec2 tp(cmin.x + (cell - ts.x) * 0.5f,
                                    cmin.y + (cell - ts.y) * 0.5f);
                    dl->AddText(tp, fg, ch.c_str(), ch.c_str() + ch.size());
                }
        }

        // Label
        ImGui::SetNextItemWidth((float)thumb_px_);
        ImGui::TextWrapped("%s", e.name.empty() ? "(unnamed)" : e.name.c_str());
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

    if (!open)
    {
        ImGui::End();
        if (session) PopImGuiWindowChromeAlpha(alpha_pushed);
        return p_open ? *p_open : false;
    }

    LoadFromSessionIfNeeded(session);

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

    // Always keep session copy updated while window is open (cheap; brush counts are small).
    SaveToSession(session);

    ImGui::End();
    if (session) PopImGuiWindowChromeAlpha(alpha_pushed);
    return p_open ? *p_open : true;
}


