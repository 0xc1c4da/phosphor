#include "ui/character_palette.h"

#include "imgui.h"
#include "core/canvas.h"
#include "core/fonts.h"
#include "core/paths.h"
#include "io/session/imgui_persistence.h"
#include "ui/imgui_window_chrome.h"
#include "ui/glyph_preview.h"
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

    // Embedded font: picker cp may be a PUA codepoint (U+E000 + glyph_index). Treat it as a selection.
    if (source_ == Source::EmbeddedFont)
    {
        if (cp >= (uint32_t)AnsiCanvas::kEmbeddedGlyphBase)
        {
            const uint32_t idx = cp - (uint32_t)AnsiCanvas::kEmbeddedGlyphBase;
            selected_cell_ = (int)idx;
            request_focus_selected_ = true;
        }
        return;
    }

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

void CharacterPalette::SyncSelectionFromActiveGlyph(uint32_t cp, const std::string& utf8, AnsiCanvas* active_canvas)
{
    EnsureLoaded();
    EnsureNonEmpty();
    active_canvas_ = active_canvas;

    // Embedded font source: select by glyph index if cp is PUA.
    if (source_ == Source::EmbeddedFont)
    {
        if (cp >= (uint32_t)AnsiCanvas::kEmbeddedGlyphBase)
        {
            const uint32_t idx = cp - (uint32_t)AnsiCanvas::kEmbeddedGlyphBase;
            selected_cell_ = (int)idx;
            request_focus_selected_ = true;
        }
        return;
    }

    // JSON palettes: prefer exact UTF-8 match (supports multi-codepoint graphemes),
    // then fall back to first codepoint match.
    if (!utf8.empty() && !palettes_.empty())
    {
        const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
        const auto& glyphs = palettes_[pi].glyphs;
        for (int i = 0; i < (int)glyphs.size(); ++i)
        {
            if (glyphs[(size_t)i].utf8 == utf8)
            {
                selected_cell_ = i;
                request_focus_selected_ = true;
                return;
            }
        }
    }

    if (auto idx = FindGlyphIndexByFirstCp(cp))
    {
        selected_cell_ = *idx;
        request_focus_selected_ = true;
    }
}

bool CharacterPalette::TakeUserSelectionChanged(GlyphToken& out_glyph, std::string& out_utf8)
{
    if (!user_selection_changed_)
        return false;
    user_selection_changed_ = false;
    out_glyph = user_selected_glyph_;
    out_utf8 = std::move(user_selected_utf8_);
    user_selected_glyph_ = GlyphToken{};
    user_selected_utf8_.clear();
    return out_glyph.IsValid();
}

bool CharacterPalette::TakeUserDoubleClicked(GlyphToken& out_glyph)
{
    if (!user_double_clicked_)
        return false;
    user_double_clicked_ = false;
    out_glyph = user_double_clicked_glyph_;
    user_double_clicked_glyph_ = GlyphToken{};
    return out_glyph.IsValid();
}

void CharacterPalette::CollectCandidateCodepoints(std::vector<uint32_t>& out, const AnsiCanvas* active_canvas) const
{
    out.clear();

    // Embedded font source: return one PUA codepoint per glyph index.
    if (source_ == Source::EmbeddedFont)
    {
        if (!active_canvas || !active_canvas->HasEmbeddedFont() || !active_canvas->GetEmbeddedFont())
            return;
        const int n = std::clamp(active_canvas->GetEmbeddedFont()->glyph_count, 0, 2048);
        out.reserve((size_t)n);
        for (int i = 0; i < n; ++i)
            out.push_back((uint32_t)AnsiCanvas::kEmbeddedGlyphBase + (uint32_t)i);
        return;
    }

    // JSON palettes: return first codepoint for each glyph in the selected palette.
    if (palettes_.empty())
        return;
    const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
    const auto& glyphs = palettes_[pi].glyphs;
    out.reserve(glyphs.size());
    for (const auto& g : glyphs)
        out.push_back(g.first_cp);
}

bool CharacterPalette::Render(const char* window_title, bool* p_open,
                              SessionState* session, bool apply_placement_this_frame,
                              AnsiCanvas* active_canvas)
{
    EnsureLoaded();
    active_canvas_ = active_canvas;

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

    // Title-bar ⋮ settings popup (in addition to the in-window collapsing header).
    {
        ImVec2 kebab_min(0.0f, 0.0f), kebab_max(0.0f, 0.0f);
        const bool has_close = (p_open != nullptr);
        const bool has_collapse = (flags & ImGuiWindowFlags_NoCollapse) == 0;
        if (RenderImGuiWindowChromeTitleBarButton("##charpal_kebab", "\xE2\x8B\xAE", has_close, has_collapse, &kebab_min, &kebab_max))
            ImGui::OpenPopup("##charpal_settings");

        if (ImGui::IsPopupOpen("##charpal_settings"))
            ImGui::SetNextWindowPos(ImVec2(kebab_min.x, kebab_max.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(780.0f, 560.0f));
        if (ImGui::BeginPopup("##charpal_settings"))
        {
            ImGui::TextUnformatted("Settings");
            ImGui::Separator();
            // Use a scrollable child to avoid huge popups for long palette lists.
            ImGui::BeginChild("##charpal_settings_scroll", ImVec2(720.0f, 420.0f), false, ImGuiWindowFlags_None);
            RenderTopBar(active_canvas);
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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

    // Settings live in the title-bar ⋮ popup.

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
    active_canvas_ = active_canvas;

    const bool has_embedded =
        (active_canvas_ && active_canvas_->HasEmbeddedFont() && active_canvas_->GetEmbeddedFont() &&
         active_canvas_->GetEmbeddedFont()->glyph_count > 0);

    // Source selection
    {
        ImGui::TextUnformatted("Source");
        ImGui::SameLine();

        const char* items[] = { "JSON Palettes", "Embedded Font (active canvas)" };
        int src = (int)source_;
        if (!has_embedded)
            src = (int)Source::JsonFile;

        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::Combo("##palette_source", &src, items, IM_ARRAYSIZE(items)))
        {
            source_ = (src == (int)Source::EmbeddedFont && has_embedded) ? Source::EmbeddedFont : Source::JsonFile;
            selected_cell_ = 0;
            request_focus_selected_ = true;
        }
        if (!has_embedded && source_ == Source::EmbeddedFont)
            source_ = Source::JsonFile;
    }

    if (source_ == Source::EmbeddedFont)
    {
        ImGui::TextUnformatted("Embedded palette is generated and read-only.");
        ImGui::Separator();
    }

    // File row
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(source_ == Source::EmbeddedFont);
    ImGui::InputText("##palette_file", &file_path_);
    ImGui::EndDisabled();

    if (!last_error_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());

    ImGui::BeginDisabled(source_ == Source::EmbeddedFont);
    if (ImGui::Button("Reload"))
        request_reload_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        request_save_ = true;
    ImGui::EndDisabled();

    ImGui::Separator();

    // Picker integration (side panel removed, keep a single toggle here).
    ImGui::TextUnformatted("Picker");
    ImGui::SameLine();
    ImGui::BeginDisabled(source_ == Source::EmbeddedFont);
    ImGui::Checkbox("Picker edits palette (replace selected cell)", &picker_replaces_selected_cell_);
    ImGui::EndDisabled();

    ImGui::Separator();

    // Palette selection
    ImGui::TextUnformatted("Palette");
    ImGui::SameLine();

    if (source_ == Source::EmbeddedFont)
    {
        ImGui::TextUnformatted("(embedded)");
        return;
    }

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
    const bool has_embedded =
        (active_canvas_ && active_canvas_->HasEmbeddedFont() && active_canvas_->GetEmbeddedFont() &&
         active_canvas_->GetEmbeddedFont()->glyph_count > 0);

    // Determine total items based on source.
    int total_items = 0;
    std::vector<Glyph>* glyphs_ptr = nullptr;
    if (source_ == Source::EmbeddedFont && has_embedded)
    {
        total_items = std::clamp(active_canvas_->GetEmbeddedFont()->glyph_count, 0, 2048);
    }
    else
    {
        source_ = Source::JsonFile;
        const int pi = std::clamp(selected_palette_, 0, (int)palettes_.size() - 1);
        glyphs_ptr = &palettes_[pi].glyphs;
        if (!glyphs_ptr || glyphs_ptr->empty())
            return;
        total_items = (int)glyphs_ptr->size();
    }

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

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

    const ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 col_sel_bg = ImGui::GetColorU32(ImGuiCol_Header);
    const ImU32 col_hover_bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    const ImU32 col_nav = ImGui::GetColorU32(ImGuiCol_NavHighlight);

    // If the active canvas uses a bitmap/embedded font, preserve its aspect in previews
    // (avoid stretching into the square cell).
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
            if (source_ == Source::EmbeddedFont && has_embedded)
            {
                user_selection_changed_ = true;
                user_selected_glyph_ = GlyphToken::EmbeddedIndex((uint32_t)idx);
                user_selected_utf8_.clear();
            }
            else if (glyphs_ptr)
            {
                const uint32_t cp = (*glyphs_ptr)[(size_t)idx].first_cp;
                if (cp != 0)
                {
                    user_selection_changed_ = true;
                    user_selected_glyph_ = GlyphToken::Unicode(cp);
                    user_selected_utf8_ = (*glyphs_ptr)[(size_t)idx].utf8;
                }
            }
        }

        // Keep keyboard caret + selection synchronized (single source of truth).
        if (focused && idx != selected_cell_)
        {
            selected_cell_ = idx;
            request_focus_selected_ = false; // already focused
            if (source_ == Source::EmbeddedFont && has_embedded)
            {
                user_selection_changed_ = true;
                user_selected_glyph_ = GlyphToken::EmbeddedIndex((uint32_t)idx);
                user_selected_utf8_.clear();
            }
            else if (glyphs_ptr)
            {
                const uint32_t cp = (*glyphs_ptr)[(size_t)idx].first_cp;
                if (cp != 0)
                {
                    user_selection_changed_ = true;
                    user_selected_glyph_ = GlyphToken::Unicode(cp);
                    user_selected_utf8_ = (*glyphs_ptr)[(size_t)idx].utf8;
                }
            }
        }
        if (double_clicked)
        {
            if (source_ == Source::EmbeddedFont && has_embedded)
            {
                user_double_clicked_ = true;
                user_double_clicked_glyph_ = GlyphToken::EmbeddedIndex((uint32_t)idx);
            }
            else if (glyphs_ptr)
            {
                const uint32_t cp = (*glyphs_ptr)[(size_t)idx].first_cp;
                if (cp != 0)
                {
                    user_double_clicked_ = true;
                    user_double_clicked_glyph_ = GlyphToken::Unicode(cp);
                }
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

        // Glyph preview:
        // - Embedded: render glyph index using the active canvas embedded font.
        // - JSON: render using the same rules as the canvas (bitmap font mapping if needed).
        char32_t cp_to_draw = U' ';
        const char* tooltip_utf8 = nullptr;
        std::string tmp;
        if (source_ == Source::EmbeddedFont && has_embedded)
        {
            cp_to_draw = (char32_t)(AnsiCanvas::kEmbeddedGlyphBase + (char32_t)idx);
            tmp = "IDX " + std::to_string(idx);
            tooltip_utf8 = tmp.c_str();
        }
        else if (glyphs_ptr)
        {
            cp_to_draw = (char32_t)(*glyphs_ptr)[(size_t)idx].first_cp;
            tooltip_utf8 = (*glyphs_ptr)[(size_t)idx].utf8.empty() ? "(empty)" : (*glyphs_ptr)[(size_t)idx].utf8.c_str();
        }

        // Fit glyph preview into the square while preserving aspect.
        float dw = cell;
        float dh = cell;
        if (preview_aspect >= 1.0f)
            dh = (preview_aspect > 0.0f) ? (cell / preview_aspect) : cell;
        else
            dw = cell * preview_aspect;
        dw = std::clamp(dw, 1.0f, cell);
        dh = std::clamp(dh, 1.0f, cell);
        const ImVec2 p(p0.x + (cell - dw) * 0.5f,
                       p0.y + (cell - dh) * 0.5f);
        DrawGlyphPreview(dl, p, dw, dh, cp_to_draw, active_canvas_, (std::uint32_t)col_text);

        if (hovered)
        {
            ImGui::BeginTooltip();
            if (source_ == Source::EmbeddedFont && has_embedded)
            {
                ImGui::Text("IDX %d", idx);
            }
            else if (glyphs_ptr)
            {
                const uint32_t cp = (*glyphs_ptr)[(size_t)idx].first_cp;
                if (cp != 0)
                    ImGui::Text("%s", CodePointHex(cp).c_str());
            }
            if (tooltip_utf8)
                ImGui::TextUnformatted(tooltip_utf8);
            ImGui::EndTooltip();
        }

        ImGui::PopID();
    }

    ImGui::PopStyleVar(2);
}


