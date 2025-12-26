#include "ui/character_set.h"

#include "imgui.h"
#include "core/canvas.h"
#include "core/fonts.h"
#include "core/i18n.h"
#include "core/paths.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"
#include "ui/glyph_preview.h"
#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

using nlohmann::json;

CharacterSetWindow::CharacterSetWindow()
{
    file_path_ = PhosphorAssetPath("character-sets.json");
}
CharacterSetWindow::~CharacterSetWindow() = default;

bool CharacterSetWindow::IsScalarValue(uint32_t cp)
{
    return (cp <= 0x10FFFFu) && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

uint32_t CharacterSetWindow::DecodeFirstCodePointUtf8(const std::string& s)
{
    if (s.empty())
        return 0;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t len = s.size();

    auto is_cont = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };

    uint32_t cp = 0;
    size_t need = 0;
    if ((p[0] & 0x80u) == 0x00u)
    {
        cp = p[0];
        need = 1;
    }
    else if ((p[0] & 0xE0u) == 0xC0u)
    {
        cp = (uint32_t)(p[0] & 0x1Fu);
        need = 2;
    }
    else if ((p[0] & 0xF0u) == 0xE0u)
    {
        cp = (uint32_t)(p[0] & 0x0Fu);
        need = 3;
    }
    else if ((p[0] & 0xF8u) == 0xF0u)
    {
        cp = (uint32_t)(p[0] & 0x07u);
        need = 4;
    }
    else
    {
        return 0;
    }

    if (need > len)
        return 0;
    for (size_t i = 1; i < need; ++i)
    {
        if (!is_cont(p[i]))
            return 0;
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }

    // Reject overlong encodings by checking minimal value for the length.
    if (need == 2 && cp < 0x80u) return 0;
    if (need == 3 && cp < 0x800u) return 0;
    if (need == 4 && cp < 0x10000u) return 0;

    if (!IsScalarValue(cp))
        return 0;
    return cp;
}

void CharacterSetWindow::DecodeAllCodePointsUtf8(const std::string& s, std::vector<uint32_t>& out)
{
    out.clear();
    if (s.empty())
        return;

    const unsigned char* data = reinterpret_cast<const unsigned char*>(s.data());
    const size_t len = s.size();
    size_t i = 0;
    while (i < len)
    {
        unsigned char c = data[i];

        uint32_t cp = 0;
        size_t remaining = 0;
        if ((c & 0x80u) == 0x00u)
        {
            cp = c;
            remaining = 0;
        }
        else if ((c & 0xE0u) == 0xC0u)
        {
            cp = c & 0x1Fu;
            remaining = 1;
        }
        else if ((c & 0xF0u) == 0xE0u)
        {
            cp = c & 0x0Fu;
            remaining = 2;
        }
        else if ((c & 0xF8u) == 0xF0u)
        {
            cp = c & 0x07u;
            remaining = 3;
        }
        else
        {
            ++i;
            continue;
        }

        if (i + remaining >= len)
            break;

        bool malformed = false;
        for (size_t j = 0; j < remaining; ++j)
        {
            const unsigned char cc = data[i + 1 + j];
            if ((cc & 0xC0u) != 0x80u)
            {
                malformed = true;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (malformed)
        {
            ++i;
            continue;
        }

        // Overlong checks:
        if (remaining == 1 && cp < 0x80u) { i += 2; continue; }
        if (remaining == 2 && cp < 0x800u) { i += 3; continue; }
        if (remaining == 3 && cp < 0x10000u) { i += 4; continue; }

        i += 1 + remaining;
        if (IsScalarValue(cp))
            out.push_back(cp);
    }
}

std::string CharacterSetWindow::EncodeCodePointUtf8(uint32_t cp)
{
    if (!IsScalarValue(cp))
        return std::string();

    char out[4];
    size_t n = 0;
    if (cp <= 0x7Fu)
    {
        out[0] = (char)cp;
        n = 1;
    }
    else if (cp <= 0x7FFu)
    {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    }
    else if (cp <= 0xFFFFu)
    {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    }
    else
    {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    return std::string(out, out + n);
}

std::string CharacterSetWindow::CodePointHex(uint32_t cp)
{
    char buf[16] = {};
    if (cp <= 0xFFFFu)
        std::snprintf(buf, sizeof(buf), "U+%04X", (unsigned)cp);
    else
        std::snprintf(buf, sizeof(buf), "U+%06X", (unsigned)cp);
    return std::string(buf);
}

void CharacterSetWindow::EnsureNonEmpty()
{
    if (!sets_.empty())
        return;

    Set s;
    s.cps.assign(12, (uint32_t)U' ');
    sets_.push_back(std::move(s));
    default_set_index_ = 0;
    active_set_index_ = 0;
    selected_slot_ = 0;
}

bool CharacterSetWindow::LoadFromFile(const char* path, std::string& error)
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
        error = "Expected JSON object in character-sets.json";
        return false;
    }

    int default_set = 0;
    if (j.contains("default_set") && j["default_set"].is_number_integer())
        default_set = j["default_set"].get<int>();

    if (!j.contains("sets") || !j["sets"].is_array())
    {
        error = "Expected 'sets' array in character-sets.json";
        return false;
    }

    std::vector<Set> parsed;
    for (const auto& item : j["sets"])
    {
        if (!item.is_string())
            continue;

        const std::string s = item.get<std::string>();
        std::vector<uint32_t> cps;
        DecodeAllCodePointsUtf8(s, cps);

        Set set;
        set.cps.assign(12, (uint32_t)U' ');
        for (int i = 0; i < 12 && i < (int)cps.size(); ++i)
            set.cps[(size_t)i] = cps[(size_t)i];
        parsed.push_back(std::move(set));
    }

    if (parsed.empty())
    {
        error = "No valid sets found in character-sets.json";
        return false;
    }

    sets_ = std::move(parsed);
    default_set_index_ = std::clamp(default_set, 0, (int)sets_.size() - 1);
    active_set_index_ = std::clamp(active_set_index_, 0, (int)sets_.size() - 1);
    selected_slot_ = std::clamp(selected_slot_, 0, 11);
    return true;
}

bool CharacterSetWindow::SaveToFile(const char* path, std::string& error) const
{
    error.clear();
    if (!path || !*path)
    {
        error = "Invalid path";
        return false;
    }

    json j;
    j["schema_version"] = 1;
    j["default_set"] = default_set_index_;
    json sets = json::array();

    for (const auto& set : sets_)
    {
        std::string out;
        out.reserve(64);
        for (size_t i = 0; i < 12; ++i)
        {
            const uint32_t cp = (i < set.cps.size()) ? set.cps[i] : (uint32_t)U' ';
            const std::string g = EncodeCodePointUtf8(cp);
            out += g.empty() ? " " : g;
        }
        sets.push_back(std::move(out));
    }
    j["sets"] = std::move(sets);

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

void CharacterSetWindow::EnsureLoaded()
{
    if (loaded_)
        return;

    std::string err;
    if (!LoadFromFile(file_path_.c_str(), err))
        last_error_ = err;
    else
        last_error_.clear();

    EnsureNonEmpty();
    active_set_index_ = std::clamp(default_set_index_, 0, std::max(0, (int)sets_.size() - 1));
    loaded_ = true;
}

bool CharacterSetWindow::SetActiveSetIndex(int idx)
{
    EnsureLoaded();
    if (sets_.empty())
        return false;
    const int clamped = std::clamp(idx, 0, (int)sets_.size() - 1);
    if (clamped == active_set_index_)
        return false;
    active_set_index_ = clamped;
    return true;
}

void CharacterSetWindow::CycleActiveSet(int delta)
{
    EnsureLoaded();
    EnsureNonEmpty();
    const int n = (int)sets_.size();
    if (n <= 0)
        return;
    int idx = active_set_index_;
    // Wrap in both directions.
    idx = ((idx + delta) % n + n) % n;
    SetActiveSetIndex(idx);
}

uint32_t CharacterSetWindow::GetSlotCodePoint(int slot_index_0_based) const
{
    if (sets_.empty())
        return (uint32_t)U' ';
    const int si = std::clamp(active_set_index_, 0, (int)sets_.size() - 1);
    const int slot = std::clamp(slot_index_0_based, 0, 11);
    const auto& cps = sets_[(size_t)si].cps;
    if (slot < 0 || slot >= (int)cps.size())
        return (uint32_t)U' ';
    return cps[(size_t)slot];
}

void CharacterSetWindow::SelectSlot(int slot_index_0_based)
{
    EnsureLoaded();
    EnsureNonEmpty();
    selected_slot_ = std::clamp(slot_index_0_based, 0, 11);
}

void CharacterSetWindow::OnExternalSelectedCodePoint(uint32_t cp)
{
    EnsureLoaded();
    EnsureNonEmpty();
    if (!edit_mode_)
        return;
    if (cp == 0 || !IsScalarValue(cp))
        return;
    selected_slot_ = std::clamp(selected_slot_, 0, 11);
    if (sets_.empty())
        return;
    const int si = std::clamp(active_set_index_, 0, (int)sets_.size() - 1);
    if ((int)sets_[(size_t)si].cps.size() != 12)
        sets_[(size_t)si].cps.assign(12, (uint32_t)U' ');
    sets_[(size_t)si].cps[(size_t)selected_slot_] = cp;
}

bool CharacterSetWindow::TakeInsertRequested(uint32_t& out_cp)
{
    if (!insert_requested_)
        return false;
    insert_requested_ = false;
    out_cp = insert_requested_cp_;
    insert_requested_cp_ = 0;
    return out_cp != 0;
}

bool CharacterSetWindow::TakeUserSelectionChanged(uint32_t& out_cp)
{
    if (!user_selection_changed_)
        return false;
    user_selection_changed_ = false;
    out_cp = user_selected_cp_;
    user_selected_cp_ = 0;
    return out_cp != 0;
}

void CharacterSetWindow::RenderTopBar(AnsiCanvas* active_canvas)
{
    active_canvas_ = active_canvas;

    // File
    ImGui::TextUnformatted(PHOS_TR("common.file").c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##charset_file", &file_path_);

    if (!last_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());

    if (ImGui::Button(PHOS_TR("common.reload").c_str()))
        request_reload_ = true;
    ImGui::SameLine();
    if (ImGui::Button(PHOS_TR("common.save").c_str()))
        request_save_ = true;

    ImGui::Separator();

    // Active set controls
    ImGui::TextUnformatted(PHOS_TR("character_sets.active_set").c_str());
    ImGui::SameLine();

    const int set_count = (int)sets_.size();
    if (set_count > 0)
        active_set_index_ = std::clamp(active_set_index_, 0, set_count - 1);

    if (ImGui::ArrowButton("##prev_set", ImGuiDir_Left) && set_count > 0)
        CycleActiveSet(-1);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next_set", ImGuiDir_Right) && set_count > 0)
        CycleActiveSet(1);
    ImGui::SameLine();

    // Combo with "Set N"
    std::vector<std::string> labels;
    labels.reserve(sets_.size());
    std::vector<const char*> items;
    items.reserve(sets_.size());
    for (int i = 0; i < (int)sets_.size(); ++i)
    {
        labels.push_back(PHOS_TRF("character_sets.tooltip_set_fmt", phos::i18n::Arg::I64((long long)i + 1)));
        items.push_back(labels.back().c_str());
    }

    ImGui::SetNextItemWidth(200.0f);
    if (!items.empty())
        ImGui::Combo("##set_combo", &active_set_index_, items.data(), (int)items.size());

    ImGui::SameLine();
    if (ImGui::Button(PHOS_TR("character_sets.make_default").c_str()))
        default_set_index_ = active_set_index_;

    ImGui::SameLine();
    ImGui::Checkbox(PHOS_TR("character_sets.edit_mode_explainer").c_str(), &edit_mode_);
}

void CharacterSetWindow::RenderSettingsContents(AnsiCanvas* active_canvas)
{
    RenderTopBar(active_canvas);

    // Selected slot actions live here (not in the always-visible grid).
    EnsureNonEmpty();
    if (!sets_.empty())
    {
        const int si = std::clamp(active_set_index_, 0, (int)sets_.size() - 1);
        auto& cps = sets_[(size_t)si].cps;
        if ((int)cps.size() != 12)
            cps.assign(12, (uint32_t)U' ');
        selected_slot_ = std::clamp(selected_slot_, 0, 11);

        ImGui::Separator();
        const uint32_t scp = cps[(size_t)selected_slot_];
        const std::string slot = PHOS_TRF("character_sets.slot_fmt",
                                          phos::i18n::Arg::I64((long long)selected_slot_ + 1),
                                          phos::i18n::Arg::Str(CodePointHex(scp)));
        ImGui::TextUnformatted(slot.c_str());
        if (ImGui::Button(PHOS_TR("character_sets.clear_slot_space").c_str()))
            cps[(size_t)selected_slot_] = (uint32_t)U' ';
        ImGui::SameLine();
        if (ImGui::Button(PHOS_TR("character_sets.insert_slot").c_str()))
        {
            insert_requested_ = true;
            insert_requested_cp_ = scp;
        }
    }
}

void CharacterSetWindow::RenderSlots()
{
    EnsureNonEmpty();
    if (sets_.empty())
        return;

    const int si = std::clamp(active_set_index_, 0, (int)sets_.size() - 1);
    auto& cps = sets_[(size_t)si].cps;
    if ((int)cps.size() != 12)
        cps.assign(12, (uint32_t)U' ');

    selected_slot_ = std::clamp(selected_slot_, 0, 11);

    // Layout: adaptive grid of 12 square buttons, scaled to fit available window area,
    // similar to ToolPalette's fit-to-window sizing.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int count = 12;

    int best_cols = 1;
    float best_size = 0.0f;
    if (count > 0 && avail.x > 1.0f)
    {
        for (int cols = 1; cols <= count; ++cols)
        {
            const float total_spacing_x = style.ItemSpacing.x * (cols - 1);
            const float width_limit = (avail.x - total_spacing_x) / (float)cols;
            if (width_limit <= 1.0f)
                break;

            const int rows = (count + cols - 1) / cols;
            float button_size = width_limit;
            if (avail.y > 1.0f)
            {
                const float total_spacing_y = style.ItemSpacing.y * (rows - 1);
                const float height_limit = (avail.y - total_spacing_y) / (float)rows;
                if (height_limit <= 1.0f)
                    continue;
                button_size = std::min(width_limit, height_limit);
            }

            if (button_size > best_size)
            {
                best_size = button_size;
                best_cols = cols;
            }
        }
    }

    // Keep a reasonable minimum so glyphs remain usable.
    const float cell = std::max(28.0f, best_size > 0.0f ? best_size : (style.FramePadding.y * 2.0f + 8.0f));
    const int cols = std::max(1, best_cols);

    // If the active canvas uses a bitmap/embedded font, preserve its aspect in previews
    // (avoid stretching into the square button).
    float preview_aspect = 1.0f; // w/h
    if (active_canvas_)
    {
        const fonts::FontInfo& finfo = fonts::Get(active_canvas_->GetFontId());
        if (const AnsiCanvas::EmbeddedBitmapFont* ef = active_canvas_->GetEmbeddedFont();
            ef && ef->cell_w > 0 && ef->cell_h > 0 && ef->glyph_count > 0 &&
            ef->bitmap.size() >= (size_t)ef->glyph_count * (size_t)ef->cell_h)
        {
            preview_aspect = (float)ef->cell_w / (float)ef->cell_h;
        }
        else if (finfo.kind == fonts::Kind::Bitmap1bpp && finfo.bitmap && finfo.cell_w > 0 && finfo.cell_h > 0)
        {
            preview_aspect = (float)finfo.cell_w / (float)finfo.cell_h;
        }
    }
    if (!(preview_aspect > 0.0f))
        preview_aspect = 1.0f;

    for (int i = 0; i < 12; ++i)
    {
        if (i % cols != 0)
            ImGui::SameLine();

        const uint32_t cp = cps[(size_t)i];

        ImGui::PushID(i);
        const bool is_sel = (i == selected_slot_);
        if (is_sel)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        // Render a normal button for interaction/styling, then overlay scaled glyph text.
        if (ImGui::Button("##slot_btn", ImVec2(cell, cell)))
        {
            selected_slot_ = i;
            user_selection_changed_ = true;
            user_selected_cp_ = cp;
        }
        {
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const float w = item_max.x - item_min.x;
            const float h = item_max.y - item_min.y;
            // Fit glyph preview into the square while preserving aspect.
            float dw = w;
            float dh = h;
            if (preview_aspect >= 1.0f)
                dh = (preview_aspect > 0.0f) ? (w / preview_aspect) : h;
            else
                dw = h * preview_aspect;
            dw = std::clamp(dw, 1.0f, w);
            dh = std::clamp(dh, 1.0f, h);
            const ImVec2 p(item_min.x + (w - dw) * 0.5f,
                           item_min.y + (h - dh) * 0.5f);
            DrawGlyphPreview(ImGui::GetWindowDrawList(),
                             p,
                             dw,
                             dh,
                             (char32_t)cp,
                             active_canvas_,
                             (std::uint32_t)ImGui::GetColorU32(ImGuiCol_Text));
        }

        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary);
        if (hovered)
        {
            ImGui::BeginTooltip();
            const std::string s0 = PHOS_TRF("character_sets.tooltip_set_fmt", phos::i18n::Arg::I64((long long)active_set_index_ + 1));
            const std::string s1 = PHOS_TRF("character_sets.tooltip_fn_fmt", phos::i18n::Arg::I64((long long)i + 1));
            ImGui::TextUnformatted(s0.c_str());
            ImGui::TextUnformatted(s1.c_str());
            ImGui::TextUnformatted(CodePointHex(cp).c_str());
            // Show something even for control chars
            if (cp < 0x20u || cp == 0x7Fu)
                ImGui::TextUnformatted(PHOS_TR("character_sets.tooltip_control").c_str());
            else
                ImGui::TextUnformatted(PHOS_TR("character_sets.tooltip_glyph_preview_matches").c_str());
            ImGui::EndTooltip();
        }

        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            insert_requested_ = true;
            insert_requested_cp_ = cp;
        }

        if (is_sel)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // No extra chrome here: the window body should be "just buttons".
}

bool CharacterSetWindow::Render(const char* window_title, bool* p_open,
                                SessionState* session, bool apply_placement_this_frame,
                                AnsiCanvas* active_canvas)
{
    EnsureLoaded();
    active_canvas_ = active_canvas;

    if (session)
        ApplyImGuiWindowPlacement(*session, window_title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings |
        (session ? GetImGuiWindowChromeExtraFlags(*session, window_title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, window_title);
    const std::string win_title = PHOS_TR("menu.window.character_sets") + "##" + std::string(window_title);
    if (!ImGui::Begin(win_title.c_str(), p_open, flags))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, window_title);
        ImGui::End();
        PopImGuiWindowChromeAlpha(alpha_pushed);
        return (p_open == nullptr) ? true : *p_open;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, window_title);
    if (session)
    {
        ApplyImGuiWindowChromeZOrder(session, window_title);
        RenderImGuiWindowChromeMenu(session, window_title);
    }

    // Title-bar ⋮ settings popup (in addition to the in-window collapsing header).
    {
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;

        // Title bar controls: [<] [>] [⋮]
        const int set_count = (int)sets_.size();
        if (RenderImGuiWindowChromeTitleBarButton("##charset_prev_set", "<", has_close, has_collapse,
                                                  /*out_rect_min=*/nullptr, /*out_rect_max=*/nullptr,
                                                  /*button_index_from_right=*/2))
        {
            if (set_count > 0)
                CycleActiveSet(-1);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(PHOS_TR("character_sets.previous_set").c_str());
            ImGui::EndTooltip();
        }

        if (RenderImGuiWindowChromeTitleBarButton("##charset_next_set", ">", has_close, has_collapse,
                                                  /*out_rect_min=*/nullptr, /*out_rect_max=*/nullptr,
                                                  /*button_index_from_right=*/1))
        {
            if (set_count > 0)
                CycleActiveSet(1);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(PHOS_TR("character_sets.next_set").c_str());
            ImGui::EndTooltip();
        }

        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        if (RenderImGuiWindowChromeTitleBarButton("##charset_kebab", "\xE2\x8B\xAE", has_close, has_collapse,
                                                  &kebab_min, &kebab_max,
                                                  /*button_index_from_right=*/0))
            ImGui::OpenPopup("##charset_settings");

        if (ImGui::IsPopupOpen("##charset_settings"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f), ImVec2(620.0f, 520.0f));
        if (ImGui::BeginPopup("##charset_settings"))
        {
            ImGui::TextUnformatted(PHOS_TR("common.settings").c_str());
            ImGui::Separator();
            RenderSettingsContents(active_canvas);
            ImGui::Separator();
            if (ImGui::Button(PHOS_TR("common.close").c_str()))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Handle queued file operations
    if (request_reload_)
    {
        request_reload_ = false;
        std::string err;
        if (!LoadFromFile(file_path_.c_str(), err))
            last_error_ = err;
        else
            last_error_.clear();
        EnsureNonEmpty();
        active_set_index_ = std::clamp(active_set_index_, 0, std::max(0, (int)sets_.size() - 1));
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

    // Extra affordance for set switching:
    // scroll mouse wheel over the window to cycle sets (when not interacting with popups).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
            if (io.MouseWheel > 0.0f)
                CycleActiveSet(1);
            else
                CycleActiveSet(-1);
        }
    }

    RenderSlots();

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return (p_open == nullptr) ? true : *p_open;
}


