#include "ui/character_palette.h"

#include "imgui.h"
#include "core/canvas.h"
#include "core/fonts.h"
#include "core/paths.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"
#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string_view>

using nlohmann::json;

namespace {

static std::string TrimCopy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

} // namespace

CharacterPalette::CharacterPalette()
{
    file_path_ = PhosphorAssetPath("character-palettes.json");
}
CharacterPalette::~CharacterPalette() = default;

uint32_t CharacterPalette::DecodeFirstCodePointUtf8(const std::string& s)
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

    // Reject invalid Unicode scalar values.
    if (cp > 0x10FFFFu) return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
    return cp;
}

std::string CharacterPalette::EncodeCodePointUtf8(uint32_t cp)
{
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
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

std::string CharacterPalette::CodePointHex(uint32_t cp)
{
    char buf[16] = {};
    if (cp <= 0xFFFFu)
        std::snprintf(buf, sizeof(buf), "U+%04X", (unsigned)cp);
    else
        std::snprintf(buf, sizeof(buf), "U+%06X", (unsigned)cp);
    return std::string(buf);
}

void CharacterPalette::EnsureNonEmpty()
{
    if (!palettes_.empty())
        return;

    Palette p;
    p.title = "Default";
    p.glyphs.push_back(Glyph{" ", DecodeFirstCodePointUtf8(" ")});
    p.glyphs.push_back(Glyph{"█", DecodeFirstCodePointUtf8("█")});
    p.glyphs.push_back(Glyph{"░", DecodeFirstCodePointUtf8("░")});
    p.glyphs.push_back(Glyph{"▒", DecodeFirstCodePointUtf8("▒")});
    p.glyphs.push_back(Glyph{"▓", DecodeFirstCodePointUtf8("▓")});
    palettes_.push_back(std::move(p));
    selected_palette_ = 0;
    selected_cell_ = 0;
}

void CharacterPalette::EnsureLoaded()
{
    if (loaded_)
        return;

    std::string err;
    if (!LoadFromFile(file_path_.c_str(), err))
        last_error_ = err;
    else
        last_error_.clear();

    EnsureNonEmpty();
    loaded_ = true;
}

bool CharacterPalette::LoadFromFile(const char* path, std::string& error)
{
    error.clear();

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

    if (!j.is_array())
    {
        error = "Expected top-level JSON array in character-palettes.json";
        return false;
    }

    std::vector<Palette> parsed;
    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        Palette p;
        if (auto it = item.find("title"); it != item.end() && it->is_string())
            p.title = it->get<std::string>();
        else
            continue;

        if (auto it = item.find("chars"); it != item.end() && it->is_array())
        {
            for (const auto& c : *it)
            {
                if (!c.is_string())
                    continue;
                Glyph g;
                g.utf8 = c.get<std::string>();
                g.first_cp = DecodeFirstCodePointUtf8(g.utf8);
                if (!g.utf8.empty())
                    p.glyphs.push_back(std::move(g));
            }
        }

        if (!p.glyphs.empty())
            parsed.push_back(std::move(p));
    }

    if (parsed.empty())
    {
        error = "No valid palettes found in character-palettes.json";
        return false;
    }

    palettes_ = std::move(parsed);
    selected_palette_ = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    selected_cell_ = 0;
    return true;
}

bool CharacterPalette::SaveToFile(const char* path, std::string& error) const
{
    error.clear();

    json out = json::array();
    for (const auto& p : palettes_)
    {
        if (p.title.empty() || p.glyphs.empty())
            continue;
        json item;
        item["title"] = p.title;
        json chars = json::array();
        for (const auto& g : p.glyphs)
        {
            if (!g.utf8.empty())
                chars.push_back(g.utf8);
        }
        item["chars"] = std::move(chars);
        out.push_back(std::move(item));
    }

    std::ofstream f(path);
    if (!f)
    {
        error = std::string("Failed to write ") + path;
        return false;
    }

    try
    {
        f << out.dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    return true;
}

uint32_t CharacterPalette::SelectedCodePoint() const
{
    if (palettes_.empty())
        return 0;
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    const auto& glyphs = palettes_[pi].glyphs;
    if (glyphs.empty())
        return 0;
    const int gi = std::clamp(selected_cell_, 0, (int)glyphs.size() - 1);
    return glyphs[gi].first_cp;
}

std::optional<int> CharacterPalette::FindGlyphIndexByFirstCp(uint32_t cp) const
{
    if (palettes_.empty())
        return std::nullopt;
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    const auto& glyphs = palettes_[pi].glyphs;
    for (int i = 0; i < (int)glyphs.size(); ++i)
        if (glyphs[i].first_cp == cp && cp != 0)
            return i;
    return std::nullopt;
}

void CharacterPalette::ReplaceSelectedCellWith(uint32_t cp)
{
    if (palettes_.empty())
        return;
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    auto& glyphs = palettes_[pi].glyphs;
    if (glyphs.empty())
        return;
    const int gi = std::clamp(selected_cell_, 0, (int)glyphs.size() - 1);

    Glyph g;
    g.utf8 = EncodeCodePointUtf8(cp);
    g.first_cp = DecodeFirstCodePointUtf8(g.utf8);
    if (g.utf8.empty())
        return;
    glyphs[gi] = std::move(g);
    request_focus_selected_ = true;
}

void CharacterPalette::OnPickerSelectedCodePoint(uint32_t cp)
{
    EnsureLoaded();
    EnsureNonEmpty();

    if (cp == 0)
        return;

    // Default: do NOT mutate the palette. Only select an existing matching glyph.
    // Optional: if enabled, picker selection replaces the currently selected cell.
    if (picker_replaces_selected_cell_)
        ReplaceSelectedCellWith(cp);
    else if (auto idx = FindGlyphIndexByFirstCp(cp))
        selected_cell_ = *idx;

    request_focus_selected_ = true;
}

bool CharacterPalette::TakeUserSelectionChanged(uint32_t& out_cp)
{
    if (!user_selection_changed_)
        return false;
    user_selection_changed_ = false;
    out_cp = user_selected_cp_;
    user_selected_cp_ = 0;
    return out_cp != 0;
}

bool CharacterPalette::TakeUserDoubleClicked(uint32_t& out_cp)
{
    if (!user_double_clicked_)
        return false;
    user_double_clicked_ = false;
    out_cp = user_double_clicked_cp_;
    user_double_clicked_cp_ = 0;
    return out_cp != 0;
}

bool CharacterPalette::Render(const char* window_title, bool* p_open,
                              SessionState* session, bool apply_placement_this_frame,
                              AnsiCanvas* active_canvas)
{
    EnsureLoaded();

    // Initialize the collapsible settings state from persisted session once.
    if (session && !settings_open_init_from_session_)
    {
        settings_open_ = session->character_palette_settings_open;
        settings_open_init_from_session_ = true;
    }

    if (session)
        ApplyImGuiWindowPlacement(*session, window_title, apply_placement_this_frame);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings |
        (session ? GetImGuiWindowChromeExtraFlags(*session, window_title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, window_title);
    if (!ImGui::Begin(window_title, p_open, flags))
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

    // Handle queued file operations (triggered by UI buttons).
    if (request_reload_)
    {
        request_reload_ = false;
        std::string err;
        if (!LoadFromFile(file_path_.c_str(), err))
            last_error_ = err;
        else
            last_error_.clear();
        EnsureNonEmpty();
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

    // Collapsible settings panel (keeps the window mostly “just the grid”).
    ImGui::SetNextItemOpen(settings_open_, ImGuiCond_Once);
    const bool open = ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_None);
    settings_open_ = open;
    if (session)
        session->character_palette_settings_open = settings_open_;
    if (open)
    {
        RenderTopBar(active_canvas);
        ImGui::Separator();
    }

    // Single full-width grid (no side editor panel).
    // Scrollbar appears only if needed (e.g. very large palettes with min cell size).
    ImGui::BeginChild("##pal_grid", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_None);
    RenderGrid();
    ImGui::EndChild();

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
    return (p_open == nullptr) ? true : *p_open;
}

void CharacterPalette::RenderTopBar(AnsiCanvas* active_canvas)
{
    (void)active_canvas;

    // File row
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##palette_file", &file_path_);

    if (!last_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());

    if (ImGui::Button("Reload"))
        request_reload_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        request_save_ = true;

    ImGui::Separator();

    // Picker integration (side panel removed, keep a single toggle here).
    ImGui::TextUnformatted("Picker");
    ImGui::SameLine();
    ImGui::Checkbox("Picker edits palette (replace selected cell)", &picker_replaces_selected_cell_);

    ImGui::Separator();

    // Palette selection
    ImGui::TextUnformatted("Palette");
    ImGui::SameLine();

    std::vector<const char*> names;
    names.reserve(palettes_.size());
    for (const auto& p : palettes_)
        names.push_back(p.title.c_str());
    selected_palette_ = std::clamp(selected_palette_, 0, std::max(0, (int)palettes_.size() - 1));

    ImGui::SetNextItemWidth(320.0f);
    if (!names.empty())
        ImGui::Combo("##palette_combo", &selected_palette_, names.data(), (int)names.size());

    ImGui::SameLine();
    if (ImGui::Button("New"))
        open_new_popup_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Rename"))
        open_rename_popup_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        open_delete_popup_ = true;

    if (open_new_popup_)
    {
        open_new_popup_ = false;
        std::snprintf(new_title_buf_, sizeof(new_title_buf_), "New Palette");
        ImGui::OpenPopup("New Palette");
    }
    if (ImGui::BeginPopupModal("New Palette", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Create a new palette.");
        ImGui::InputText("Title", new_title_buf_, IM_ARRAYSIZE(new_title_buf_));
        if (ImGui::Button("Create"))
        {
            Palette p;
            p.title = TrimCopy(new_title_buf_);
            if (p.title.empty())
                p.title = "Untitled";
            // Start with 256 blanks to feel like a "grid".
            p.glyphs.resize(256);
            for (auto& g : p.glyphs)
            {
                g.utf8 = " ";
                g.first_cp = DecodeFirstCodePointUtf8(g.utf8);
            }
            palettes_.push_back(std::move(p));
            selected_palette_ = (int)palettes_.size() - 1;
            selected_cell_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (open_rename_popup_)
    {
        open_rename_popup_ = false;
        const int pi = std::clamp(selected_palette_, 0, std::max(0, (int)palettes_.size() - 1));
        const std::string cur = palettes_.empty() ? std::string() : palettes_[pi].title;
        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", cur.c_str());
        ImGui::OpenPopup("Rename Palette");
    }
    if (ImGui::BeginPopupModal("Rename Palette", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Rename the current palette.");
        ImGui::InputText("Title", rename_buf_, IM_ARRAYSIZE(rename_buf_));
        if (ImGui::Button("OK"))
        {
            if (!palettes_.empty())
            {
                const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
                const std::string t = TrimCopy(rename_buf_);
                if (!t.empty())
                    palettes_[pi].title = t;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (open_delete_popup_)
    {
        open_delete_popup_ = false;
        ImGui::OpenPopup("Delete Palette?");
    }
    if (ImGui::BeginPopupModal("Delete Palette?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Delete the current palette? This cannot be undone.");
        if (ImGui::Button("Delete"))
        {
            if (!palettes_.empty())
            {
                const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
                palettes_.erase(palettes_.begin() + pi);
                if (palettes_.empty())
                    EnsureNonEmpty();
                selected_palette_ = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
                selected_cell_ = 0;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void CharacterPalette::RenderGrid()
{
    EnsureNonEmpty();
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    auto& glyphs = palettes_[pi].glyphs;
    if (glyphs.empty())
        return;

    // Tight grid with scalable glyph rendering (no IDX column, no header row).
    const int total_items = (int)glyphs.size();
    selected_cell_ = std::clamp(selected_cell_, 0, std::max(0, total_items - 1));

    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Fit-to-window sizing (similar to the adaptive grid logic used by the colour palette):
    // Choose the column count that maximizes cell size while fitting in available width/height.
    int best_cols = 1;
    float best_cell = 0.0f;
    if (total_items > 0 && avail.x > 1.0f)
    {
        const int max_cols_to_try = std::min(total_items, 256);
        for (int cols = 1; cols <= max_cols_to_try; ++cols)
        {
            const float width_limit = avail.x / (float)cols;
            if (width_limit <= 1.0f)
                break;
            const int rows = (total_items + cols - 1) / cols;

            float cell = width_limit;
            if (avail.y > 1.0f)
            {
                const float height_limit = avail.y / (float)rows;
                if (height_limit <= 1.0f)
                    continue;
                cell = std::min(width_limit, height_limit);
            }

            if (cell > best_cell)
            {
                best_cell = cell;
                best_cols = cols;
            }
        }
    }

    // If "perfect fit" makes cells too tiny, prefer a reasonable minimum size and allow scrolling.
    constexpr float kMinCell = 14.0f;
    constexpr float kMaxCell = 256.0f;
    int cols = best_cols;
    float cell = std::clamp(best_cell, 1.0f, kMaxCell);
    if (cell < kMinCell)
    {
        cell = kMinCell;
        cols = (avail.x > cell) ? std::max(1, (int)std::floor(avail.x / cell)) : 1;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

    const ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 col_sel_bg = ImGui::GetColorU32(ImGuiCol_Header);
    const ImU32 col_hover_bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    const ImU32 col_nav = ImGui::GetColorU32(ImGuiCol_NavHighlight);

    for (int idx = 0; idx < total_items; ++idx)
    {
        const int c = idx % cols;
        if (c != 0)
            ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushID(idx);

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + cell, p0.y + cell);

        // Enable ImGui keyboard navigation for this custom grid so arrow keys move the "caret".
        // We then mirror nav-focus -> selected_cell_ to keep the two highlights synchronized.
        ImGui::InvisibleButton("##cell", ImVec2(cell, cell), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary);
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool focused = ImGui::IsItemFocused();
        const bool double_clicked =
            hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        const bool is_sel = (idx == selected_cell_);
        if (clicked)
        {
            selected_cell_ = idx;
            request_focus_selected_ = true;
            const uint32_t cp = glyphs[(size_t)idx].first_cp;
            if (cp != 0)
            {
                user_selection_changed_ = true;
                user_selected_cp_ = cp;
            }
        }

        // Keep keyboard caret + selection synchronized (single source of truth).
        if (focused && idx != selected_cell_)
        {
            selected_cell_ = idx;
            request_focus_selected_ = false; // already focused
            const uint32_t cp = glyphs[(size_t)idx].first_cp;
            if (cp != 0)
            {
                user_selection_changed_ = true;
                user_selected_cp_ = cp;
            }
        }
        if (double_clicked)
        {
            const uint32_t cp = glyphs[(size_t)idx].first_cp;
            if (cp != 0)
            {
                user_double_clicked_ = true;
                user_double_clicked_cp_ = cp;
            }
        }

        if (is_sel)
            dl->AddRectFilled(p0, p1, col_sel_bg);
        else if (hovered)
            dl->AddRectFilled(p0, p1, col_hover_bg);

        // Draw a visible keyboard "caret" when this cell is nav-focused.
        if (focused)
            dl->AddRect(p0, p1, col_nav, 0.0f, 0, 2.0f);

        // If selection changed programmatically (or via mouse), request nav focus on selected cell
        // so we don't end up with a second caret elsewhere.
        if (request_focus_selected_ &&
            idx == selected_cell_ &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            ImGui::SetItemDefaultFocus();
            request_focus_selected_ = false;
        }

        const std::string& glyph = glyphs[(size_t)idx].utf8;
        const char* text = glyph.empty() ? " " : glyph.c_str();

        // Scale glyph drawing with cell size (no window-wide font scaling).
        float font_px = cell * 0.85f;
        if (font_px < 6.0f) font_px = 6.0f;
        ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, text, nullptr);
        const float max_dim = cell * 0.92f;
        if (ts.x > max_dim && ts.x > 0.0f) font_px *= (max_dim / ts.x);
        if (ts.y > max_dim && ts.y > 0.0f) font_px *= (max_dim / ts.y);
        ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, text, nullptr);

        const ImVec2 tp(p0.x + (cell - ts.x) * 0.5f,
                        p0.y + (cell - ts.y) * 0.5f);
        dl->AddText(font, font_px, tp, col_text, text, nullptr);

        if (hovered)
        {
            const uint32_t cp = glyphs[(size_t)idx].first_cp;
            ImGui::BeginTooltip();
            if (cp != 0)
                ImGui::Text("%s", CodePointHex(cp).c_str());
            ImGui::TextUnformatted(glyph.empty() ? "(empty)" : glyph.c_str());
            ImGui::EndTooltip();
        }

        ImGui::PopID();
    }

    ImGui::PopStyleVar(2);
}


