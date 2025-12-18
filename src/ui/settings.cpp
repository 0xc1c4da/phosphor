#include "ui/settings.h"

#include "imgui.h"
#include "core/paths.h"
#include "core/key_bindings.h"
#include "io/session/imgui_persistence.h"
#include "ui/skin.h"
#include "ui/imgui_window_chrome.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool IContains(const std::string& haystack, const std::string& needle_lower)
{
    if (needle_lower.empty())
        return true;
    return ToLower(haystack).find(needle_lower) != std::string::npos;
}

static int PlatformIndex(const std::string& p)
{
    if (p == "windows") return 1;
    if (p == "linux") return 2;
    if (p == "macos") return 3;
    return 0;
}

static std::string PlatformFromIndex(int idx)
{
    switch (idx)
    {
        case 1: return "windows";
        case 2: return "linux";
        case 3: return "macos";
        default: return "any";
    }
}

static int ContextIndex(const std::string& c)
{
    if (c == "editor") return 1;
    if (c == "selection") return 2;
    if (c == "canvas") return 3;
    return 0;
}

static std::string ContextFromIndex(int idx)
{
    switch (idx)
    {
        case 1: return "editor";
        case 2: return "selection";
        case 3: return "canvas";
        default: return "global";
    }
}

static bool IsModifierKey(ImGuiKey key)
{
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
           key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
           key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
           // This Dear ImGui version exposes "reserved" entries used internally
           // for mod tracking; treat them as modifiers for capture purposes.
           key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift ||
           key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModSuper;
}

static std::string NormalizeKeyName(std::string s)
{
    // ImGui key names are generally fine, but a few are nicer in editor terminology.
    if (s == "LeftArrow") return "Left";
    if (s == "RightArrow") return "Right";
    if (s == "UpArrow") return "Up";
    if (s == "DownArrow") return "Down";
    if (s == "PageUp") return "PageUp";
    if (s == "PageDown") return "PageDown";
    if (s == "KeypadEnter") return "Enter";
    return s;
}

static std::string BuildChordString(const ImGuiIO& io, ImGuiKey key)
{
    std::string out;
    if (io.KeyCtrl)  out += "Ctrl+";
    if (io.KeyShift) out += "Shift+";
    if (io.KeyAlt)   out += "Alt+";
#if defined(__APPLE__)
    if (io.KeySuper) out += "Cmd+";
#else
    if (io.KeySuper) out += "Super+";
#endif

    const char* name = ImGui::GetKeyName(key);
    std::string key_name = name ? name : "";
    key_name = NormalizeKeyName(key_name);
    out += key_name.empty() ? "Unknown" : key_name;
    return out;
}
} // namespace

SettingsWindow::SettingsWindow()
{
    // Key bindings are backed by a shared core engine (attached via SetKeyBindingsEngine()).
}

void SettingsWindow::RegisterTab(const Tab& tab)
{
    // Replace if same id exists.
    for (auto& t : tabs_)
    {
        if (t.id == tab.id)
        {
            t = tab;
            return;
        }
    }
    tabs_.push_back(tab);
}

void SettingsWindow::EnsureDefaultTabsRegistered()
{
    if (tabs_registered_)
        return;
    tabs_registered_ = true;

    RegisterTab(Tab{
        .id = "skin",
        .title = "Skin",
        .render = [this]() { RenderTab_Skin(); },
    });

    RegisterTab(Tab{
        .id = "key_bindings",
        .title = "Key Bindings",
        .render = [this]() { RenderTab_KeyBindings(); },
    });

    // Placeholder future tabs: keep the UI structure extensible.
    RegisterTab(Tab{
        .id = "about",
        .title = "About",
        .render = []()
        {
            ImGui::TextUnformatted("Phosphor");
            ImGui::Separator();
            ImGui::TextUnformatted("Settings tabs are designed to be extendable.");
        },
    });
}

void SettingsWindow::Render(const char* title, SessionState* session, bool apply_placement_this_frame)
{
    if (!open_)
        return;

    EnsureDefaultTabsRegistered();
    session_ = session;

    // Provide a reasonable default size for first-time users, but prefer persisted placements.
    if (session && apply_placement_this_frame)
    {
        auto it = session->imgui_windows.find(title);
        const bool has = (it != session->imgui_windows.end() && it->second.valid);
        if (!has)
            ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_Always);
    }
    else if (!session)
    {
        ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
    }

    if (session)
        ApplyImGuiWindowPlacement(*session, title, apply_placement_this_frame);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_None |
        (session ? GetImGuiWindowChromeExtraFlags(*session, title) : ImGuiWindowFlags_None);
    const bool alpha_pushed = PushImGuiWindowChromeAlpha(session, title);
    if (!ImGui::Begin(title, &open_, flags))
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

    if (ImGui::BeginTabBar("##settings_tabs"))
    {
        for (auto& tab : tabs_)
        {
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (!active_tab_id_.empty() && tab.id == active_tab_id_)
                flags |= ImGuiTabItemFlags_SetSelected;

            if (ImGui::BeginTabItem(tab.title.c_str(), nullptr, flags))
            {
                active_tab_id_ = tab.id;
                if (tab.render)
                    tab.render();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    PopImGuiWindowChromeAlpha(alpha_pushed);
}

void SettingsWindow::RenderTab_Skin()
{
    if (!session_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Session state not attached; cannot persist theme.");
        return;
    }

    ImGui::TextUnformatted("Theme");
    ImGui::Separator();

    ImGui::SetNextItemWidth(260.0f);
    const char* current_label = ui::ThemeDisplayName(session_->ui_theme.c_str());
    if (ImGui::BeginCombo("##theme", current_label))
    {
        for (int i = 0; i < ui::ThemeCount(); ++i)
        {
            const char* id = ui::ThemeIdByIndex(i);
            const bool selected = (session_->ui_theme == id);
            if (ImGui::Selectable(ui::ThemeDisplayName(id), selected))
            {
                session_->ui_theme = id;
                ui::ApplyTheme(session_->ui_theme.c_str(), main_scale_);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Theme is saved in session.json and restored on startup.");

    const char* default_id = ui::DefaultThemeId();
    const std::string reset_label =
        std::string("Reset to default (") + ui::ThemeDisplayName(default_id) + ")";
    if (ImGui::Button(reset_label.c_str()))
    {
        session_->ui_theme = default_id;
        ui::ApplyTheme(session_->ui_theme.c_str(), main_scale_);
    }
}

void SettingsWindow::RenderTab_KeyBindings()
{
    if (!keybinds_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Key bindings engine not attached.");
        return;
    }

    // Lazy load (from the engine's configured path).
    if (!keybinds_->IsLoaded())
    {
        std::string err;
        (void)keybinds_->LoadFromFile(keybinds_->Path(), err);
    }

    // Header row: file path + dirty indicator + actions
    {
        ImGui::Text("File: %s", keybinds_->Path().c_str());
        ImGui::SameLine();
        if (keybinds_->IsDirty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "• Modified");
        }
    }

    if (!keybinds_->LastError().empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", keybinds_->LastError().c_str());
    }

    ImGui::Separator();

    // Controls
    {
        if (ImGui::Button("Reload"))
        {
            std::string err;
            keybinds_->LoadFromFile(keybinds_->Path(), err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            std::string err;
            if (keybinds_->SaveToFile(keybinds_->Path(), err))
            {
                keybinds_->ClearDirty();
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show IDs", &show_ids_);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##kb_filter", "Filter actions…", &filter_text_);
    }

    ImGui::Separator();

    // Record binding modal (UI only; writes chord string into the selected binding).
    if (capture_active_)
        ImGui::OpenPopup("Record Key Binding");

    if (ImGui::BeginPopupModal("Record Key Binding", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGuiIO& io = ImGui::GetIO();

        ImGui::TextUnformatted("Press a key to assign this binding.");
        ImGui::TextDisabled("Held modifiers will be included (Ctrl/Shift/Alt/Super).");
        ImGui::TextDisabled("Escape: cancel   Backspace/Delete: clear");
        ImGui::Separator();

        // Live preview while holding modifiers (without committing until a non-mod key is pressed).
        {
            std::string mods;
            if (io.KeyCtrl)  mods += "Ctrl+";
            if (io.KeyShift) mods += "Shift+";
            if (io.KeyAlt)   mods += "Alt+";
            if (io.KeySuper) mods += "Super+";
            if (mods.empty()) mods = "(no modifiers)";
            ImGui::Text("Modifiers: %s", mods.c_str());
        }

        bool close = false;
        bool commit = false;
        std::string committed_chord;

        // Cancel
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            close = true;
        }

        // Clear (and close)
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            commit = true;
            committed_chord.clear();
            close = true;
        }

        // Capture next pressed non-mod key.
        if (!close)
        {
            for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = (ImGuiKey)(key + 1))
            {
                if (!ImGui::IsKeyPressed(key, false))
                    continue;
                if (IsModifierKey(key))
                    continue;

                commit = true;
                committed_chord = BuildChordString(io, key);
                close = true;
                break;
            }
        }

        if (ImGui::Button("Cancel"))
            close = true;
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            commit = true;
            committed_chord.clear();
            close = true;
        }

        auto& actions = keybinds_->ActionsMutable();
        if (commit && capture_action_idx_ < actions.size())
        {
            auto& a = actions[capture_action_idx_];
            if (capture_binding_idx_ < a.bindings.size())
            {
                a.bindings[capture_binding_idx_].chord = committed_chord;
                keybinds_->MarkDirty();
            }
        }

        if (close)
        {
            capture_active_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Sort a view (stable) by category/title for nicer display.
    const auto& actions_ro = keybinds_->Actions();
    std::vector<size_t> order(actions_ro.size());
    for (size_t i = 0; i < actions_ro.size(); ++i)
        order[i] = i;

    std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib)
    {
        const auto& a = actions_ro[ia];
        const auto& b = actions_ro[ib];
        if (a.category != b.category) return a.category < b.category;
        return a.title < b.title;
    });

    const std::string needle = ToLower(filter_text_);

    if (ImGui::BeginTable("##kb_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Action column was previously too wide; keep it compact.
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, 0.66f);
        ImGui::TableHeadersRow();

        std::string last_cat;
        for (size_t oi = 0; oi < order.size(); ++oi)
        {
            const size_t idx = order[oi];
            // Mutations go through the engine.
            auto& a = keybinds_->ActionsMutable()[idx];

            // Filter match on category/title/id/description.
            if (!needle.empty())
            {
                if (!IContains(a.title, needle) &&
                    !IContains(a.category, needle) &&
                    !IContains(a.id, needle) &&
                    !IContains(a.description, needle))
                    continue;
            }

            // Category separator row
            if (a.category != last_cat)
            {
                last_cat = a.category;
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(last_cat.c_str());
                ImGui::TableNextColumn();
            }

            ImGui::PushID((int)idx);
            ImGui::TableNextRow();

            // Action column
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.title.c_str());
            if (show_ids_)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", a.id.c_str());
            }
            if (!a.description.empty())
            {
                ImGui::TextDisabled("%s", a.description.c_str());
            }

            // Bindings column
            ImGui::TableNextColumn();
            for (size_t bi = 0; bi < a.bindings.size(); ++bi)
            {
                kb::KeyBinding& b = a.bindings[bi];
                ImGui::PushID((int)bi);

                // enabled
                if (ImGui::Checkbox("##en", &b.enabled))
                    keybinds_->MarkDirty();
                ImGui::SameLine();

                // platform
                {
                    const char* items[] = { "Any", "Windows", "Linux", "macOS" };
                    int pidx = PlatformIndex(b.platform);
                    // Slightly wider than before (was too cramped).
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::Combo("##plat", &pidx, items, IM_ARRAYSIZE(items)))
                    {
                        b.platform = PlatformFromIndex(pidx);
                        keybinds_->MarkDirty();
                    }
                }
                ImGui::SameLine();

                // context
                {
                    const char* items[] = { "Global", "Editor", "Selection", "Canvas" };
                    int cidx = ContextIndex(b.context);
                    // Slightly wider than before (was too cramped).
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::Combo("##ctx", &cidx, items, IM_ARRAYSIZE(items)))
                    {
                        b.context = ContextFromIndex(cidx);
                        keybinds_->MarkDirty();
                    }
                }
                ImGui::SameLine();

                // chord
                // Chord input was too wide; keep it compact so inline buttons are always visible.
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputTextWithHint("##chord", "e.g. Ctrl+Z", &b.chord))
                    keybinds_->MarkDirty();

                // Inline controls on the same row as chord input.
                ImGui::SameLine();
                if (ImGui::SmallButton("Add"))
                {
                    kb::KeyBinding nb;
                    nb.enabled = true;
                    nb.platform = "any";
                    nb.context = "global";
                    nb.chord.clear();
                    a.bindings.push_back(std::move(nb));
                    keybinds_->MarkDirty();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Record…"))
                {
                    capture_active_ = true;
                    capture_action_idx_ = idx;
                    capture_binding_idx_ = bi;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                {
                    a.bindings.erase(a.bindings.begin() + (ptrdiff_t)bi);
                    keybinds_->MarkDirty();
                    ImGui::PopID();
                    break; // binding list mutated; restart next frame
                }

                ImGui::PopID();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}


