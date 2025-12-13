#include "character_palette.h"

#include "imgui.h"
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

CharacterPalette::CharacterPalette() = default;
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
        error = "Expected top-level JSON array in palettes.json";
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
        error = "No valid palettes found in palettes.json";
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
}

void CharacterPalette::OnPickerSelectedCodePoint(uint32_t cp)
{
    EnsureLoaded();
    EnsureNonEmpty();

    if (cp == 0)
        return;

    if (!picker_replaces_selected_cell_)
    {
        if (auto idx = FindGlyphIndexByFirstCp(cp))
        {
            selected_cell_ = *idx;
            return;
        }
    }

    ReplaceSelectedCellWith(cp);
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

bool CharacterPalette::Render(const char* window_title, bool* p_open)
{
    EnsureLoaded();

    if (!ImGui::Begin(window_title, p_open, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        return (p_open == nullptr) ? true : *p_open;
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

    RenderTopBar();
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float editor_w = 340.0f;
    const float grid_w = std::max(200.0f, avail.x - editor_w - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("##pal_grid", ImVec2(grid_w, 0.0f), true);
    RenderGrid();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##pal_editor", ImVec2(0.0f, 0.0f), true);
    RenderEditorPanel();
    ImGui::EndChild();

    ImGui::End();
    return (p_open == nullptr) ? true : *p_open;
}

void CharacterPalette::RenderTopBar()
{
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

    constexpr int kCols = 16;
    const float cell_w = 26.0f;
    const float rowhdr_w = 56.0f;

    const int total_cols = 1 + kCols;
    ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInner | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY;

    ImVec2 outer_size(0.0f, std::max(1.0f, ImGui::GetContentRegionAvail().y));
    if (!ImGui::BeginTable("##palette_table", total_cols, flags, outer_size))
        return;

    ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed, rowhdr_w);
    for (int c = 0; c < kCols; ++c)
    {
        char hdr[8];
        std::snprintf(hdr, sizeof(hdr), "%X", c);
        ImGui::TableSetupColumn(hdr, ImGuiTableColumnFlags_WidthFixed, cell_w);
    }
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableHeadersRow();

    const int total_items = (int)glyphs.size();
    const int row_count = (total_items + (kCols - 1)) / kCols;

    ImGuiListClipper clipper;
    clipper.Begin(row_count);
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char idxbuf[16];
            std::snprintf(idxbuf, sizeof(idxbuf), "%04X", r * kCols);
            ImGui::TextUnformatted(idxbuf);

            for (int c = 0; c < kCols; ++c)
            {
                ImGui::TableSetColumnIndex(1 + c);
                const int idx = r * kCols + c;
                if (idx >= total_items)
                {
                    ImGui::TextUnformatted("");
                    continue;
                }

                const bool is_sel = (idx == selected_cell_);
                const std::string& glyph = glyphs[(size_t)idx].utf8;
                const char* label = glyph.empty() ? " " : glyph.c_str();

                ImGui::PushID(idx);
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
                if (ImGui::Selectable(label, is_sel, ImGuiSelectableFlags_None, ImVec2(cell_w, cell_w)))
                {
                    selected_cell_ = idx;
                    const uint32_t cp = glyphs[(size_t)idx].first_cp;
                    if (cp != 0)
                    {
                        user_selection_changed_ = true;
                        user_selected_cp_ = cp;
                    }
                }
                ImGui::PopStyleVar();

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
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
        }
    }

    ImGui::EndTable();
}

void CharacterPalette::RenderEditorPanel()
{
    EnsureNonEmpty();
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    auto& glyphs = palettes_[pi].glyphs;
    if (glyphs.empty())
        return;

    selected_cell_ = std::clamp(selected_cell_, 0, (int)glyphs.size() - 1);
    Glyph& g = glyphs[(size_t)selected_cell_];

    ImGui::TextUnformatted("Selected Cell");
    ImGui::Separator();

    ImGui::Text("Index: %d", selected_cell_);
    if (g.first_cp != 0)
        ImGui::Text("%s", CodePointHex(g.first_cp).c_str());
    else
        ImGui::TextDisabled("Invalid codepoint");

    ImGui::Separator();

    ImGui::TextUnformatted("Glyph (UTF-8)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##glyph_utf8", &g.utf8))
        g.first_cp = DecodeFirstCodePointUtf8(g.utf8);

    if (ImGui::Button("Copy Glyph") && !g.utf8.empty())
        ImGui::SetClipboardText(g.utf8.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy U+XXXX") && g.first_cp != 0)
    {
        const std::string hex = CodePointHex(g.first_cp);
        ImGui::SetClipboardText(hex.c_str());
    }

    ImGui::Separator();

    ImGui::TextUnformatted("Picker Integration");
    ImGui::Checkbox("Picker selection replaces selected cell", &picker_replaces_selected_cell_);

    ImGui::Separator();

    ImGui::TextUnformatted("Palette Editing");

    if (ImGui::Button("Insert Blank After"))
    {
        Glyph blank{" ", DecodeFirstCodePointUtf8(" ")};
        glyphs.insert(glyphs.begin() + selected_cell_ + 1, blank);
        selected_cell_ += 1;
    }
    if (ImGui::Button("Delete Cell"))
    {
        if (!glyphs.empty())
        {
            glyphs.erase(glyphs.begin() + selected_cell_);
            if (glyphs.empty())
                glyphs.push_back(Glyph{" ", DecodeFirstCodePointUtf8(" ")});
            selected_cell_ = std::clamp(selected_cell_, 0, (int)glyphs.size() - 1);
        }
    }

    ImGui::TextDisabled("Tip: select a character in the Unicode picker to replace this cell.");
}


