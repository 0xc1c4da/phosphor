#include "settings.h"

#include "imgui.h"
#include "imgui_persistence.h"
#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>

using json = nlohmann::json;

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
    if (io.KeySuper) out += "Super+";

    const char* name = ImGui::GetKeyName(key);
    std::string key_name = name ? name : "";
    key_name = NormalizeKeyName(key_name);
    out += key_name.empty() ? "Unknown" : key_name;
    return out;
}

static json KeyBindingToJson(const SettingsWindow::KeyBinding& b)
{
    json jb;
    jb["enabled"] = b.enabled;
    jb["chord"] = b.chord;
    jb["context"] = b.context.empty() ? "global" : b.context;
    jb["platform"] = b.platform.empty() ? "any" : b.platform;
    return jb;
}

static bool KeyBindingFromJson(const json& jb, SettingsWindow::KeyBinding& out, std::string& err)
{
    err.clear();
    if (!jb.is_object())
    {
        err = "binding is not an object";
        return false;
    }
    out = SettingsWindow::KeyBinding{};
    if (jb.contains("enabled") && jb["enabled"].is_boolean())
        out.enabled = jb["enabled"].get<bool>();
    if (jb.contains("chord") && jb["chord"].is_string())
        out.chord = jb["chord"].get<std::string>();
    if (jb.contains("context") && jb["context"].is_string())
        out.context = jb["context"].get<std::string>();
    else
        out.context = "global";
    if (jb.contains("platform") && jb["platform"].is_string())
        out.platform = jb["platform"].get<std::string>();
    else
        out.platform = "any";

    // Minimal validation: chord must be non-empty for an enabled binding.
    if (out.enabled && out.chord.empty())
    {
        err = "binding chord is empty";
        return false;
    }
    if (out.context.empty()) out.context = "global";
    if (out.platform.empty()) out.platform = "any";
    return true;
}

static json ActionToJson(const SettingsWindow::Action& a)
{
    json ja;
    ja["id"] = a.id;
    ja["title"] = a.title;
    ja["category"] = a.category;
    if (!a.description.empty())
        ja["description"] = a.description;
    json binds = json::array();
    for (const auto& b : a.bindings)
        binds.push_back(KeyBindingToJson(b));
    ja["bindings"] = std::move(binds);
    return ja;
}

static bool ActionFromJson(const json& ja, SettingsWindow::Action& out, std::string& err)
{
    err.clear();
    if (!ja.is_object())
    {
        err = "action is not an object";
        return false;
    }
    if (!ja.contains("id") || !ja["id"].is_string())
    {
        err = "action missing string 'id'";
        return false;
    }

    out = SettingsWindow::Action{};
    out.id = ja["id"].get<std::string>();
    if (ja.contains("title") && ja["title"].is_string())
        out.title = ja["title"].get<std::string>();
    if (ja.contains("category") && ja["category"].is_string())
        out.category = ja["category"].get<std::string>();
    if (ja.contains("description") && ja["description"].is_string())
        out.description = ja["description"].get<std::string>();

    if (out.title.empty())
        out.title = out.id;
    if (out.category.empty())
        out.category = "Other";

    out.bindings.clear();
    if (ja.contains("bindings") && ja["bindings"].is_array())
    {
        for (const auto& jb : ja["bindings"])
        {
            SettingsWindow::KeyBinding b;
            std::string berr;
            if (!KeyBindingFromJson(jb, b, berr))
            {
                err = "action '" + out.id + "': " + berr;
                return false;
            }
            out.bindings.push_back(std::move(b));
        }
    }

    return true;
}
} // namespace

SettingsWindow::SettingsWindow()
{
    // Delay loading until first render to avoid file IO during startup.
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

bool SettingsWindow::LoadKeyBindingsFromFile(const std::string& path, std::string& out_error)
{
    out_error.clear();
    keybindings_path_ = path;

    std::ifstream f(path);
    if (!f)
    {
        // If missing, fall back to defaults and consider it "loaded" so UI works.
        actions_ = DefaultActions();
        loaded_ = true;
        dirty_ = true; // prompt user to save new file
        last_error_ = std::string("Could not open '") + path + "'. Using defaults (not saved yet).";
        out_error = last_error_;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        actions_ = DefaultActions();
        loaded_ = true;
        dirty_ = true;
        last_error_ = std::string("JSON parse error: ") + e.what();
        out_error = last_error_;
        return false;
    }

    if (!j.is_object())
    {
        out_error = "key-bindings.json root must be an object";
        last_error_ = out_error;
        return false;
    }
    if (!j.contains("schema_version") || !j["schema_version"].is_number_integer())
    {
        out_error = "key-bindings.json missing integer 'schema_version'";
        last_error_ = out_error;
        return false;
    }
    const int ver = j["schema_version"].get<int>();
    if (ver != 1)
    {
        out_error = "Unsupported key-bindings schema_version (expected 1)";
        last_error_ = out_error;
        return false;
    }
    if (!j.contains("actions") || !j["actions"].is_array())
    {
        out_error = "key-bindings.json missing 'actions' array";
        last_error_ = out_error;
        return false;
    }

    std::vector<Action> actions;
    for (const auto& ja : j["actions"])
    {
        Action a;
        std::string err;
        if (!ActionFromJson(ja, a, err))
        {
            out_error = err;
            last_error_ = out_error;
            return false;
        }
        actions.push_back(std::move(a));
    }

    actions_ = std::move(actions);
    loaded_ = true;
    dirty_ = false;
    last_error_.clear();
    return true;
}

bool SettingsWindow::SaveKeyBindingsToFile(const std::string& path, std::string& out_error) const
{
    out_error.clear();

    json j;
    j["schema_version"] = 1;
    j["name"] = "Phosphor Key Bindings";
    j["description"] = "Action->key mapping for Phosphor. Chords are human-readable strings (e.g. Ctrl+Z).";
    j["notes"] = json::array({
        "This file is intended to be edited in-app via File > Settings > Key Bindings.",
        "Fields are forward-compatible: unknown fields should be preserved by future loaders.",
    });

    json actions = json::array();
    for (const auto& a : actions_)
        actions.push_back(ActionToJson(a));
    j["actions"] = std::move(actions);

    std::ofstream out(path);
    if (!out)
    {
        out_error = "Failed to open file for writing.";
        return false;
    }

    try
    {
        out << j.dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        out_error = std::string("Failed to write JSON: ") + e.what();
        return false;
    }

    return true;
}

void SettingsWindow::EnsureDefaultTabsRegistered()
{
    if (tabs_registered_)
        return;
    tabs_registered_ = true;

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

    if (!ImGui::Begin(title, &open_, ImGuiWindowFlags_None))
    {
        if (session)
            CaptureImGuiWindowPlacement(*session, title);
        ImGui::End();
        return;
    }
    if (session)
        CaptureImGuiWindowPlacement(*session, title);

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
}

void SettingsWindow::RenderTab_KeyBindings()
{
    // Lazy load
    if (!loaded_)
    {
        std::string err;
        (void)LoadKeyBindingsFromFile(keybindings_path_, err);
    }

    // Header row: file path + dirty indicator + actions
    {
        ImGui::Text("File: %s", keybindings_path_.c_str());
        ImGui::SameLine();
        if (dirty_)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "• Modified");
        }
    }

    if (!last_error_.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error_.c_str());
    }

    ImGui::Separator();

    // Controls
    {
        if (ImGui::Button("Reload"))
        {
            std::string err;
            LoadKeyBindingsFromFile(keybindings_path_, err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            std::string err;
            if (SaveKeyBindingsToFile(keybindings_path_, err))
            {
                // Mark clean only on success.
                dirty_ = false;
                last_error_.clear();
            }
            else
            {
                last_error_ = err.empty() ? "Save failed." : err;
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

        if (commit && capture_action_idx_ < actions_.size())
        {
            auto& a = actions_[capture_action_idx_];
            if (capture_binding_idx_ < a.bindings.size())
            {
                a.bindings[capture_binding_idx_].chord = committed_chord;
                dirty_ = true;
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
    std::vector<size_t> order(actions_.size());
    for (size_t i = 0; i < actions_.size(); ++i)
        order[i] = i;

    std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib)
    {
        const auto& a = actions_[ia];
        const auto& b = actions_[ib];
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
            Action& a = actions_[idx];

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
                KeyBinding& b = a.bindings[bi];
                ImGui::PushID((int)bi);

                // enabled
                if (ImGui::Checkbox("##en", &b.enabled))
                    dirty_ = true;
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
                        dirty_ = true;
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
                        dirty_ = true;
                    }
                }
                ImGui::SameLine();

                // chord
                // Chord input was too wide; keep it compact so inline buttons are always visible.
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputTextWithHint("##chord", "e.g. Ctrl+Z", &b.chord))
                    dirty_ = true;

                // Inline controls on the same row as chord input.
                ImGui::SameLine();
                if (ImGui::SmallButton("Add"))
                {
                    KeyBinding nb;
                    nb.enabled = true;
                    nb.platform = "any";
                    nb.context = "global";
                    nb.chord.clear();
                    a.bindings.push_back(std::move(nb));
                    dirty_ = true;
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
                    dirty_ = true;
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

std::vector<SettingsWindow::Action> SettingsWindow::DefaultActions()
{
    // Seeded primarily from references/hotkeys.md "Common keybindings (cross-editor comparison)".
    // This is a curated set of common concepts; bindings include platform variants where known.
    return {
        // --- File ---
        {
            .id="app.file.new", .title="New", .category="File",
            .description="Create a new canvas/document.",
            .bindings={
                {.enabled=true, .chord="Ctrl+N", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+N", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.open", .title="Open…", .category="File",
            .description="Open a file/project from disk.",
            .bindings={
                {.enabled=true, .chord="Ctrl+O", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+O", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.save", .title="Save", .category="File",
            .description="Save the current document/project.",
            .bindings={
                {.enabled=true, .chord="Ctrl+S", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+S", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.save_as", .title="Save As…", .category="File",
            .description="Save a copy / choose format.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Shift+S", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+Shift+S", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.close_window", .title="Close Window", .category="File",
            .description="Close the current window.",
            .bindings={
                {.enabled=true, .chord="Ctrl+W", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+W", .context="global", .platform="macos"},
                {.enabled=true, .chord="Alt+F4", .context="global", .platform="windows"},
            }
        },
        {
            .id="app.quit", .title="Quit", .category="File",
            .description="Exit the application.",
            .bindings={
                {.enabled=true, .chord="Alt+X", .context="global", .platform="windows"},
                {.enabled=true, .chord="Cmd+Q", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.settings.open", .title="Settings…", .category="File",
            .description="Open the Settings window.",
            .bindings={
                {.enabled=true, .chord="Ctrl+,", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+,", .context="global", .platform="macos"},
            }
        },

        // --- Edit ---
        {
            .id="edit.undo", .title="Undo", .category="Edit",
            .description="Undo last operation.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Z", .context="editor", .platform="any"},
                {.enabled=true, .chord="Cmd+Z", .context="editor", .platform="macos"},
            }
        },
        {
            .id="edit.redo", .title="Redo", .category="Edit",
            .description="Redo last undone operation.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Shift+Z", .context="editor", .platform="any"},
                {.enabled=true, .chord="Ctrl+Y", .context="editor", .platform="windows"},
                {.enabled=true, .chord="Cmd+Shift+Z", .context="editor", .platform="macos"},
            }
        },
        {
            .id="edit.cut", .title="Cut", .category="Edit",
            .description="Cut selection to clipboard.",
            .bindings={
                {.enabled=true, .chord="Ctrl+X", .context="selection", .platform="any"},
                {.enabled=true, .chord="Cmd+X", .context="selection", .platform="macos"},
            }
        },
        {
            .id="edit.copy", .title="Copy", .category="Edit",
            .description="Copy selection to clipboard.",
            .bindings={
                {.enabled=true, .chord="Ctrl+C", .context="selection", .platform="any"},
                {.enabled=true, .chord="Cmd+C", .context="selection", .platform="macos"},
            }
        },
        {
            .id="edit.paste", .title="Paste", .category="Edit",
            .description="Paste clipboard at caret/cursor.",
            .bindings={
                {.enabled=true, .chord="Ctrl+V", .context="editor", .platform="any"},
                {.enabled=true, .chord="Cmd+V", .context="editor", .platform="macos"},
                // Icy Draw default differs (Ctrl+L); included for compatibility.
                {.enabled=false, .chord="Ctrl+L", .context="editor", .platform="any"},
            }
        },
        {
            .id="edit.select_all", .title="Select All", .category="Edit",
            .description="Select the full canvas/document extent.",
            .bindings={
                {.enabled=true, .chord="Ctrl+A", .context="editor", .platform="any"},
                {.enabled=true, .chord="Cmd+A", .context="editor", .platform="macos"},
            }
        },

        // --- Selection ---
        {
            .id="selection.clear_or_cancel", .title="Clear Selection / Cancel", .category="Selection",
            .description="Clear selection or cancel current selection operation.",
            .bindings={
                {.enabled=true, .chord="Escape", .context="selection", .platform="any"},
            }
        },
        {
            .id="selection.delete", .title="Delete Selection Contents", .category="Selection",
            .description="Erase selection contents.",
            .bindings={
                {.enabled=true, .chord="Delete", .context="selection", .platform="any"},
            }
        },
        {
            .id="selection.start_block", .title="Start Selection / Block Select", .category="Selection",
            .description="Start a selection (block select).",
            .bindings={
                {.enabled=true, .chord="Alt+B", .context="editor", .platform="any"},
            }
        },

        // --- Navigation / caret ---
        {
            .id="nav.caret_left", .title="Move Caret Left", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Left", .context="editor", .platform="any"} }
        },
        {
            .id="nav.caret_right", .title="Move Caret Right", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Right", .context="editor", .platform="any"} }
        },
        {
            .id="nav.caret_up", .title="Move Caret Up", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Up", .context="editor", .platform="any"} }
        },
        {
            .id="nav.caret_down", .title="Move Caret Down", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Down", .context="editor", .platform="any"} }
        },
        {
            .id="nav.select_left", .title="Extend Selection Left", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Shift+Left", .context="editor", .platform="any"} }
        },
        {
            .id="nav.select_right", .title="Extend Selection Right", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Shift+Right", .context="editor", .platform="any"} }
        },
        {
            .id="nav.select_up", .title="Extend Selection Up", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Shift+Up", .context="editor", .platform="any"} }
        },
        {
            .id="nav.select_down", .title="Extend Selection Down", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Shift+Down", .context="editor", .platform="any"} }
        },
        {
            .id="nav.home", .title="Line Start", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="Home", .context="editor", .platform="any"} }
        },
        {
            .id="nav.end", .title="Line End", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="End", .context="editor", .platform="any"} }
        },
        {
            .id="nav.page_up", .title="Page Up", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="PageUp", .context="editor", .platform="any"} }
        },
        {
            .id="nav.page_down", .title="Page Down", .category="Navigation", .description="",
            .bindings={ {.enabled=true, .chord="PageDown", .context="editor", .platform="any"} }
        },

        // --- Editing (text-like) ---
        {
            .id="editor.toggle_insert", .title="Toggle Insert Mode", .category="Editor", .description="",
            .bindings={ {.enabled=true, .chord="Insert", .context="editor", .platform="any"} }
        },
        {
            .id="editor.new_line", .title="New Line", .category="Editor", .description="",
            .bindings={ {.enabled=true, .chord="Enter", .context="editor", .platform="any"} }
        },
        {
            .id="editor.backspace", .title="Backspace", .category="Editor", .description="",
            .bindings={ {.enabled=true, .chord="Backspace", .context="editor", .platform="any"} }
        },

        // --- Colors / attributes ---
        {
            .id="color.prev_fg", .title="Previous Foreground Color", .category="Color", .description="",
            .bindings={ {.enabled=true, .chord="Ctrl+Up", .context="editor", .platform="any"} }
        },
        {
            .id="color.next_fg", .title="Next Foreground Color", .category="Color", .description="",
            .bindings={ {.enabled=true, .chord="Ctrl+Down", .context="editor", .platform="any"} }
        },
        {
            .id="color.prev_bg", .title="Previous Background Color", .category="Color", .description="",
            .bindings={ {.enabled=true, .chord="Ctrl+Left", .context="editor", .platform="any"} }
        },
        {
            .id="color.next_bg", .title="Next Background Color", .category="Color", .description="",
            .bindings={ {.enabled=true, .chord="Ctrl+Right", .context="editor", .platform="any"} }
        },
        {
            .id="color.pick_attribute", .title="Pick Attribute Under Caret", .category="Color", .description="",
            .bindings={ {.enabled=true, .chord="Alt+U", .context="editor", .platform="any"} }
        },
        {
            .id="color.default", .title="Default Color", .category="Color", .description="",
            .bindings={
                {.enabled=true, .chord="Ctrl+D", .context="editor", .platform="any"},
                {.enabled=true, .chord="Cmd+D", .context="editor", .platform="macos"},
            }
        },

        // --- View ---
        {
            .id="view.zoom_in", .title="Zoom In", .category="View", .description="",
            .bindings={
                {.enabled=true, .chord="Ctrl+=", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+=", .context="global", .platform="macos"},
                {.enabled=true, .chord="Ctrl++", .context="global", .platform="any"},
            }
        },
        {
            .id="view.zoom_out", .title="Zoom Out", .category="View", .description="",
            .bindings={
                {.enabled=true, .chord="Ctrl+-", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+-", .context="global", .platform="macos"},
            }
        },
        {
            .id="view.zoom_reset", .title="Reset Zoom", .category="View", .description="",
            .bindings={
                {.enabled=true, .chord="Ctrl+0", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+0", .context="global", .platform="macos"},
            }
        },
    };
}


