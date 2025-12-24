#include "ui/settings.h"

#include "imgui.h"
#include "core/color_system.h"
#include "core/paths.h"
#include "core/key_bindings.h"
#include "io/session/imgui_persistence.h"
#include "ui/skin.h"
#include "ui/imgui_window_chrome.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
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
        .id = "general",
        .title = "General",
        .render = [this]() { RenderTab_General(); },
    });

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
            ImGui::TextUnformatted("Phosphor by 0xc1c4da");
            ImGui::Separator();
            ImGui::TextUnformatted("A native UTF-8 ANSI / text-mode art editor based on the Unscii 8x16 font.");
          
        },
    });
}

void SettingsWindow::RenderTab_General()
{
    if (!session_)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Session state not attached; cannot persist settings.");
        return;
    }

    ImGui::TextUnformatted("Undo History");
    ImGui::Separator();

    bool unlimited = (session_->undo_limit == 0);
    bool changed = false;

    if (ImGui::Checkbox("Unlimited undo history", &unlimited))
    {
        changed = true;
        session_->undo_limit = unlimited ? 0 : 4096; // reasonable default when enabling a cap
    }

    if (!unlimited)
    {
        int v = (session_->undo_limit > 0) ? (int)session_->undo_limit : 4096;
        v = std::clamp(v, 1, 1000000);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputInt("Max undo steps", &v, 64, 512))
        {
            v = std::clamp(v, 1, 1000000);
            session_->undo_limit = (size_t)v;
            changed = true;
        }

        // These preset buttons use numeric labels that also appear elsewhere in this window.
        // Scope them with a unique ID to avoid ImGui ID collisions.
        ImGui::PushID("undo_limit_presets");
        ImGui::SameLine();
        if (ImGui::SmallButton("256"))
        {
            session_->undo_limit = 256;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1024"))
        {
            session_->undo_limit = 1024;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("4096"))
        {
            session_->undo_limit = 4096;
            changed = true;
        }
        ImGui::PopID();

        ImGui::Spacing();
        ImGui::TextDisabled("Tip: large values can use a lot of memory for big canvases.");
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Unlimited keeps all undo snapshots in memory (can grow large).");
    }

    if (changed && undo_limit_applier_)
        undo_limit_applier_(session_->undo_limit);

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextUnformatted("Zoom");
    ImGui::Separator();
    {
        // Applies to all canvases; the app propagates this setting each frame.
        int mode = session_->zoom_snap_mode;
        if (mode == 0) mode = 2; // migrated default (Auto -> Pixel-aligned)
        mode = std::clamp(mode, 1, 2);
        const char* items[] = {
            "Integer scale (N\xC3\x97)",
            "Pixel-aligned cell width",
        };
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::Combo("Zoom snapping", &mode, items, IM_ARRAYSIZE(items)))
        {
            session_->zoom_snap_mode = std::clamp(mode, 1, 2);
        }
        ImGui::TextDisabled(
            "Integer: always snap to integer zoom steps.\n"
            "Pixel-aligned: always snap by cell width (can introduce artifacts for bitmap fonts).");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextUnformatted("LUT Cache");
    ImGui::Separator();

    {
        auto format_mib = [](std::size_t bytes) -> float {
            return (bytes > 0) ? ((float)bytes / (1024.0f * 1024.0f)) : 0.0f;
        };

        // Exposed as MiB for humans; stored as bytes.
        int mib = (session_->lut_cache_budget_bytes > 0)
                    ? (int)(session_->lut_cache_budget_bytes / (1024ull * 1024ull))
                    : 0;

        bool lut_changed = false;

        bool unlimited_lut = (session_->lut_cache_budget_bytes == 0);
        if (ImGui::Checkbox("Unlimited LUT cache", &unlimited_lut))
        {
            session_->lut_cache_budget_bytes = unlimited_lut ? 0 : (64ull * 1024ull * 1024ull);
            lut_changed = true;
        }

        if (!unlimited_lut)
        {
            mib = std::clamp(mib <= 0 ? 64 : mib, 1, 1024);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputInt("Max LUT cache (MiB)", &mib, 8, 32))
            {
                mib = std::clamp(mib, 1, 1024);
                session_->lut_cache_budget_bytes = (size_t)mib * 1024ull * 1024ull;
                lut_changed = true;
            }

            // Scope preset buttons to avoid collisions with other numeric presets in this window.
            ImGui::PushID("lut_budget_presets");
            ImGui::SameLine();
            if (ImGui::SmallButton("32"))
            {
                session_->lut_cache_budget_bytes = 32ull * 1024ull * 1024ull;
                lut_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("64"))
            {
                session_->lut_cache_budget_bytes = 64ull * 1024ull * 1024ull;
                lut_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("96"))
            {
                session_->lut_cache_budget_bytes = 96ull * 1024ull * 1024ull;
                lut_changed = true;
            }
            ImGui::PopID();
        }

        if (lut_changed && lut_cache_budget_applier_)
            lut_cache_budget_applier_(session_->lut_cache_budget_bytes);

        // Budget pressure indicator (live): 100% corresponds to the current allocatable budget.
        {
            auto& cs = phos::color::GetColorSystem();
            const std::size_t used_b = cs.Luts().UsedBytes();
            const std::size_t budget_b = cs.Luts().BudgetBytes();

            ImGui::Spacing();
            ImGui::TextUnformatted("Budget pressure");

            if (budget_b > 0)
            {
                const float frac = (budget_b > 0) ? (float)((double)used_b / (double)budget_b) : 0.0f;
                char label[128];
                std::snprintf(label, sizeof(label), "%.1f / %.1f MiB (%.0f%%)",
                              format_mib(used_b), format_mib(budget_b),
                              (double)std::clamp(frac, 0.0f, 1.0f) * 100.0);
                ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), label);
            }
            else
            {
                char label[128];
                std::snprintf(label, sizeof(label), "%.1f MiB used (unlimited budget)", format_mib(used_b));
                ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0.0f), label);
            }
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextUnformatted("Glyph Atlas Cache");
    ImGui::Separator();

    {
        auto format_mib = [](std::size_t bytes) -> float {
            return (bytes > 0) ? ((float)bytes / (1024.0f * 1024.0f)) : 0.0f;
        };

        int mib = (session_->glyph_atlas_cache_budget_bytes > 0)
                    ? (int)(session_->glyph_atlas_cache_budget_bytes / (1024ull * 1024ull))
                    : 0;

        bool atlas_changed = false;
        bool unlimited_atlas = (session_->glyph_atlas_cache_budget_bytes == 0);
        if (ImGui::Checkbox("Unlimited glyph atlas cache", &unlimited_atlas))
        {
            session_->glyph_atlas_cache_budget_bytes = unlimited_atlas ? 0 : (96ull * 1024ull * 1024ull);
            atlas_changed = true;
        }

        if (!unlimited_atlas)
        {
            mib = std::clamp(mib <= 0 ? 96 : mib, 1, 2048);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputInt("Max glyph atlas cache (MiB)", &mib, 8, 32))
            {
                mib = std::clamp(mib, 1, 2048);
                session_->glyph_atlas_cache_budget_bytes = (size_t)mib * 1024ull * 1024ull;
                atlas_changed = true;
            }

            // Scope preset buttons to avoid collisions with other numeric presets in this window.
            ImGui::PushID("glyph_atlas_budget_presets");
            ImGui::SameLine();
            if (ImGui::SmallButton("32"))
            {
                session_->glyph_atlas_cache_budget_bytes = 32ull * 1024ull * 1024ull;
                atlas_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("64"))
            {
                session_->glyph_atlas_cache_budget_bytes = 64ull * 1024ull * 1024ull;
                atlas_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("96"))
            {
                session_->glyph_atlas_cache_budget_bytes = 96ull * 1024ull * 1024ull;
                atlas_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("128"))
            {
                session_->glyph_atlas_cache_budget_bytes = 128ull * 1024ull * 1024ull;
                atlas_changed = true;
            }
            ImGui::PopID();
        }

        if (atlas_changed && glyph_atlas_cache_budget_applier_)
            glyph_atlas_cache_budget_applier_(session_->glyph_atlas_cache_budget_bytes);

        // Budget pressure indicator (live): 100% corresponds to the current budget.
        {
            const std::size_t used_b = glyph_atlas_cache_used_bytes_getter_ ? glyph_atlas_cache_used_bytes_getter_() : 0;
            const std::size_t budget_b = session_->glyph_atlas_cache_budget_bytes;

            ImGui::Spacing();
            ImGui::TextUnformatted("Budget pressure");

            if (budget_b > 0)
            {
                const float frac = (budget_b > 0) ? (float)((double)used_b / (double)budget_b) : 0.0f;
                char label[128];
                std::snprintf(label, sizeof(label), "%.1f / %.1f MiB (%.0f%%)",
                              format_mib(used_b), format_mib(budget_b),
                              (double)std::clamp(frac, 0.0f, 1.0f) * 100.0);
                ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), label);
            }
            else
            {
                char label[128];
                std::snprintf(label, sizeof(label), "%.1f MiB used (unlimited budget)", format_mib(used_b));
                ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0.0f), label);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Tip: this caches bitmap font atlases for fast/correct rendering across many open canvases.");
        }
    }
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
            // Don't force selection every frame: doing so prevents the user from switching tabs.
            // Let ImGui manage selection; we just observe which tab is active.
            if (ImGui::BeginTabItem(tab.title.c_str()))
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

    // ---------------------------------------------------------------------
    // Collision / invalid-binding report
    // ---------------------------------------------------------------------
    // Goal: surface ambiguous keybindings so users can resolve them without guesswork.
    //
    // Notes:
    // - The runtime engine can allow multiple contexts at once (global+editor+canvas+selection),
    //   so identical chords across contexts can fire multiple actions in the same frame.
    // - Platform "any" overlaps all concrete platforms.
    // - We compare chords by parsed/normalized representation (mods + key + "any_enter").
    struct ChordSig
    {
        int  key = (int)ImGuiKey_None;
        bool any_enter = false;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        bool super = false;

        bool operator==(const ChordSig& o) const
        {
            return key == o.key &&
                   any_enter == o.any_enter &&
                   ctrl == o.ctrl &&
                   shift == o.shift &&
                   alt == o.alt &&
                   super == o.super;
        }
    };
    struct ChordSigHash
    {
        size_t operator()(const ChordSig& s) const
        {
            size_t h = 1469598103934665603ull;
            auto mix = [&](size_t v) {
                h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            };
            mix((size_t)s.key);
            mix((size_t)s.any_enter);
            mix((size_t)s.ctrl);
            mix((size_t)s.shift);
            mix((size_t)s.alt);
            mix((size_t)s.super);
            return h;
        }
    };
    struct BindingRef
    {
        size_t action_idx = 0;
        size_t binding_idx = 0;
        std::string action_id;
        std::string action_title;
        std::string chord_text;
        std::string context;
    };
    auto platform_expansion = [](const std::string& p) -> std::vector<std::string_view>
    {
        if (p == "any" || p.empty())
            return { "windows", "linux", "macos" };
        if (p == "windows") return { "windows" };
        if (p == "linux") return { "linux" };
        if (p == "macos") return { "macos" };
        // Unknown: treat as "any" for collision visibility.
        return { "windows", "linux", "macos" };
    };

    std::string conflicts_report;
    {
        const auto& actions_ro = keybinds_->Actions();

        // Key: (platform, chord signature) -> all bindings using it.
        struct Key
        {
            std::string platform;
            ChordSig sig;
            bool operator==(const Key& o) const { return platform == o.platform && sig == o.sig; }
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const
            {
                size_t h = std::hash<std::string>{}(k.platform);
                h ^= ChordSigHash{}(k.sig) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                return h;
            }
        };

        std::unordered_map<Key, std::vector<BindingRef>, KeyHash> groups;
        groups.reserve(actions_ro.size() * 2);

        std::vector<std::string> invalid;

        for (size_t ai = 0; ai < actions_ro.size(); ++ai)
        {
            const auto& a = actions_ro[ai];
            for (size_t bi = 0; bi < a.bindings.size(); ++bi)
            {
                const auto& b = a.bindings[bi];
                if (!b.enabled)
                    continue;
                if (b.chord.empty())
                    continue;

                kb::ParsedChord pc;
                std::string perr;
                if (!kb::ParseChordString(b.chord, pc, perr))
                {
                    invalid.push_back(a.id + " (" + a.title + "): '" + b.chord + "' -> " + perr);
                    continue;
                }

                ChordSig sig;
                sig.key = (int)pc.key;
                sig.any_enter = pc.any_enter;
                sig.ctrl = pc.mods.ctrl;
                sig.shift = pc.mods.shift;
                sig.alt = pc.mods.alt;
                sig.super = pc.mods.super;

                BindingRef ref;
                ref.action_idx = ai;
                ref.binding_idx = bi;
                ref.action_id = a.id;
                ref.action_title = a.title;
                ref.chord_text = b.chord;
                ref.context = b.context.empty() ? "global" : b.context;

                for (std::string_view plat : platform_expansion(b.platform))
                {
                    Key k;
                    k.platform = std::string(plat);
                    k.sig = sig;
                    groups[k].push_back(ref);
                }
            }
        }

        // Build human-readable report.
        // Keep it stable-ish by sorting keys by platform then chord string.
        struct GroupOut
        {
            std::string platform;
            std::string chord;
            std::vector<BindingRef> refs;
        };
        std::vector<GroupOut> outs;
        outs.reserve(groups.size());
        for (auto& it : groups)
        {
            auto& refs = it.second;
            if (refs.size() < 2)
                continue;
            // Prefer sorting refs by category-ish: action title then id then context.
            std::sort(refs.begin(), refs.end(), [](const BindingRef& a, const BindingRef& b) {
                if (a.action_title != b.action_title) return a.action_title < b.action_title;
                if (a.action_id != b.action_id) return a.action_id < b.action_id;
                return a.context < b.context;
            });

            GroupOut g;
            g.platform = it.first.platform;
            // Use the first chord string as a representative label (may differ in spelling but same parsed chord).
            g.chord = refs.front().chord_text;
            g.refs = refs;
            outs.push_back(std::move(g));
        }

        std::sort(outs.begin(), outs.end(), [](const GroupOut& a, const GroupOut& b) {
            if (a.platform != b.platform) return a.platform < b.platform;
            return a.chord < b.chord;
        });

        if (!outs.empty() || !invalid.empty())
        {
            // Compact report:
            // One line per collision, grouping platforms when the collision set is the same.
            conflicts_report.reserve(4096);

            // Build merge key: (chord, refs list) -> platforms set
            struct MergeKey
            {
                std::string chord;
                std::string refs_key;
                bool operator==(const MergeKey& o) const { return chord == o.chord && refs_key == o.refs_key; }
            };
            struct MergeKeyHash
            {
                size_t operator()(const MergeKey& k) const
                {
                    size_t h = std::hash<std::string>{}(k.chord);
                    h ^= std::hash<std::string>{}(k.refs_key) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                    return h;
                }
            };
            std::unordered_map<MergeKey, std::vector<std::string>, MergeKeyHash> merged_platforms;
            std::unordered_map<MergeKey, std::vector<BindingRef>, MergeKeyHash> merged_refs;

            for (const auto& g : outs)
            {
                std::string refs_key;
                refs_key.reserve(g.refs.size() * 48);
                for (const auto& r : g.refs)
                {
                    refs_key += r.action_id;
                    refs_key += "|";
                    refs_key += r.context;
                    refs_key += ";";
                }

                MergeKey mk;
                mk.chord = g.chord;
                mk.refs_key = std::move(refs_key);
                merged_platforms[mk].push_back(g.platform);
                merged_refs[mk] = g.refs;
            }

            // Emit collisions (sorted).
            struct Row { std::string chord; std::vector<std::string> plats; std::vector<BindingRef> refs; };
            std::vector<Row> rows;
            rows.reserve(merged_platforms.size());
            for (auto& it : merged_platforms)
            {
                Row r;
                r.chord = it.first.chord;
                r.plats = std::move(it.second);
                std::sort(r.plats.begin(), r.plats.end());
                r.plats.erase(std::unique(r.plats.begin(), r.plats.end()), r.plats.end());
                r.refs = merged_refs[it.first];
                rows.push_back(std::move(r));
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if (a.chord != b.chord) return a.chord < b.chord;
                return a.plats < b.plats;
            });

            if (!rows.empty())
            {
                conflicts_report += "Conflicts:\n";
                for (const auto& r : rows)
                {
                    // platforms joined by '/'
                    std::string plats;
                    for (size_t i = 0; i < r.plats.size(); ++i)
                    {
                        if (i) plats += "/";
                        plats += r.plats[i];
                    }
                    conflicts_report += r.chord + " [" + plats + "]: ";
                    for (size_t i = 0; i < r.refs.size(); ++i)
                    {
                        const auto& br = r.refs[i];
                        if (i) conflicts_report += ", ";
                        conflicts_report += br.action_id;
                        conflicts_report += " (";
                        conflicts_report += br.context;
                        conflicts_report += ")";
                    }
                    conflicts_report += "\n";
                }
            }

            if (!invalid.empty())
            {
                if (!conflicts_report.empty()) conflicts_report += "\n";
                conflicts_report += "Invalid chords:\n";
                std::sort(invalid.begin(), invalid.end());
                invalid.erase(std::unique(invalid.begin(), invalid.end()), invalid.end());
                for (const auto& s : invalid)
                {
                    conflicts_report += s;
                    conflicts_report += "\n";
                }
            }
        }
    }

    if (!conflicts_report.empty())
    {
        ImGui::SeparatorText("Keybinding Conflicts");
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Read-only multiline textbox (easy to copy/paste).
        ImGui::InputTextMultiline("##kb_conflicts", &conflicts_report,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 7.0f),
                                  ImGuiInputTextFlags_ReadOnly);
        ImGui::Separator();
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

                // repeat (hold to repeat)
                {
                    if (ImGui::Checkbox("##repeat", &b.repeat))
                    {
                        // Mark as explicitly set so it survives default-inheritance semantics.
                        b.repeat_set = true;
                        keybinds_->MarkDirty();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGui::SetTooltip(
                            "Repeat while held.\n"
                            "When enabled, holding the chord will retrigger after a short delay\n"
                            "and then repeat at a steady rate (uses ImGui key repeat timing).");
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("Rpt");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGui::SetTooltip(
                            "Repeat while held.\n"
                            "Enable for navigation/backspace/delete; disable for one-shot actions.");
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


