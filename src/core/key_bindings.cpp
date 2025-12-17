#include "core/key_bindings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <unordered_set>

using json = nlohmann::json;

namespace kb
{
namespace
{
static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string Trim(std::string s)
{
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back()))
        s.pop_back();
    return s;
}

static Platform PlatformFromString(const std::string& p)
{
    const std::string pl = ToLower(p);
    if (pl == "windows") return Platform::Windows;
    if (pl == "linux") return Platform::Linux;
    if (pl == "macos") return Platform::MacOS;
    return Platform::Any;
}

static Context ContextFromString(const std::string& c)
{
    const std::string cl = ToLower(c);
    if (cl == "editor") return Context::Editor;
    if (cl == "selection") return Context::Selection;
    if (cl == "canvas") return Context::Canvas;
    return Context::Global;
}

static bool ContextAllowed(Context need, const EvalContext& have)
{
    switch (need)
    {
        case Context::Global: return have.global;
        case Context::Editor: return have.editor;
        case Context::Selection: return have.selection;
        case Context::Canvas: return have.canvas;
        default: return false;
    }
}

static bool PlatformAllowed(Platform need, Platform have)
{
    return need == Platform::Any || need == have;
}

static ImGuiKey KeyFromToken(const std::string& token_lower, bool& out_any_enter, bool& out_implied_shift)
{
    out_any_enter = false;
    out_implied_shift = false;

    const std::string t = token_lower;

    // Single-character alpha/digit
    if (t.size() == 1)
    {
        const char c = t[0];
        if (c >= 'a' && c <= 'z')
            return (ImGuiKey)((int)ImGuiKey_A + (c - 'a'));
        if (c >= '0' && c <= '9')
            return (ImGuiKey)((int)ImGuiKey_0 + (c - '0'));
    }

    // Function keys F1..F24
    if (t.size() >= 2 && t[0] == 'f')
    {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24)
            return (ImGuiKey)((int)ImGuiKey_F1 + (n - 1));
    }

    if (t == "left") return ImGuiKey_LeftArrow;
    if (t == "right") return ImGuiKey_RightArrow;
    if (t == "up") return ImGuiKey_UpArrow;
    if (t == "down") return ImGuiKey_DownArrow;
    if (t == "home") return ImGuiKey_Home;
    if (t == "end") return ImGuiKey_End;
    if (t == "pageup") return ImGuiKey_PageUp;
    if (t == "pagedown") return ImGuiKey_PageDown;
    if (t == "insert") return ImGuiKey_Insert;
    if (t == "delete") return ImGuiKey_Delete;
    if (t == "backspace") return ImGuiKey_Backspace;
    if (t == "escape" || t == "esc") return ImGuiKey_Escape;
    if (t == "tab") return ImGuiKey_Tab;
    if (t == "space") return ImGuiKey_Space;

    if (t == "enter" || t == "return")
    {
        out_any_enter = true;
        return ImGuiKey_Enter;
    }

    // Common punctuation used in bindings.
    if (t == "," || t == "comma") return ImGuiKey_Comma;
    if (t == "-" || t == "minus") return ImGuiKey_Minus;
    if (t == "=" || t == "equal") return ImGuiKey_Equal;

    // "Plus" is usually Shift+'=' on US layouts; represent as '=' with implied Shift.
    if (t == "+" || t == "plus")
    {
        out_implied_shift = true;
        return ImGuiKey_Equal;
    }

    // "Cmd" is handled as a modifier token, not a key token.
    return ImGuiKey_None;
}

static bool ModExactMatch(const Mods& m, const ImGuiIO& io)
{
    // Require exact modifier match: avoids Ctrl+Shift+Z also triggering Ctrl+Z, etc.
    if ((bool)io.KeyCtrl != m.ctrl) return false;
    if ((bool)io.KeyShift != m.shift) return false;
    if ((bool)io.KeyAlt != m.alt) return false;
    if ((bool)io.KeySuper != m.super) return false;
    return true;
}

static bool IsChordPressed(const ParsedChord& chord, const ImGuiIO& io)
{
    if (chord.key == ImGuiKey_None)
        return false;
    if (!ModExactMatch(chord.mods, io))
        return false;
    if (chord.any_enter)
        return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    return ImGui::IsKeyPressed(chord.key, false);
}

static bool KeyBindingToJson(const KeyBinding& b, json& out)
{
    out = json::object();
    out["enabled"] = b.enabled;
    out["chord"] = b.chord;
    out["context"] = b.context.empty() ? "global" : b.context;
    out["platform"] = b.platform.empty() ? "any" : b.platform;
    return true;
}

static bool KeyBindingFromJson(const json& jb, KeyBinding& out, std::string& err)
{
    err.clear();
    if (!jb.is_object())
    {
        err = "binding is not an object";
        return false;
    }
    out = KeyBinding{};
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

    if (out.context.empty()) out.context = "global";
    if (out.platform.empty()) out.platform = "any";

    if (out.enabled && out.chord.empty())
    {
        err = "binding chord is empty";
        return false;
    }
    return true;
}

static bool ActionFromJson(const json& ja, Action& out, std::string& err)
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

    out = Action{};
    out.id = ja["id"].get<std::string>();
    if (ja.contains("title") && ja["title"].is_string())
        out.title = ja["title"].get<std::string>();
    if (ja.contains("category") && ja["category"].is_string())
        out.category = ja["category"].get<std::string>();
    if (ja.contains("description") && ja["description"].is_string())
        out.description = ja["description"].get<std::string>();

    if (out.title.empty()) out.title = out.id;
    if (out.category.empty()) out.category = "Other";

    out.bindings.clear();
    if (ja.contains("bindings") && ja["bindings"].is_array())
    {
        for (const auto& jb : ja["bindings"])
        {
            KeyBinding b;
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

static json ActionToJson(const Action& a)
{
    json ja;
    ja["id"] = a.id;
    ja["title"] = a.title;
    ja["category"] = a.category;
    if (!a.description.empty())
        ja["description"] = a.description;
    json binds = json::array();
    for (const auto& b : a.bindings)
    {
        json jb;
        KeyBindingToJson(b, jb);
        binds.push_back(std::move(jb));
    }
    ja["bindings"] = std::move(binds);
    return ja;
}
} // namespace

bool ParseChordString(const std::string& chord, ParsedChord& out, std::string& err)
{
    err.clear();
    out = ParsedChord{};

    std::string s = Trim(chord);
    if (s.empty())
    {
        err = "empty chord";
        return false;
    }

    // Split on '+' but keep empty tokens (so "Ctrl++" yields a "+" key token).
    std::vector<std::string> parts;
    {
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '+')
            {
                parts.push_back(cur);
                cur.clear();
                continue;
            }
            cur.push_back(c);
        }
        parts.push_back(cur);
    }

    Mods mods;
    std::optional<ImGuiKey> key;
    bool any_enter = false;

    for (size_t i = 0; i < parts.size(); ++i)
    {
        std::string tok = Trim(parts[i]);
        if (tok.empty())
            tok = "+"; // "Ctrl++" -> treat empty token as plus key.
        const std::string tl = ToLower(tok);

        if (tl == "ctrl" || tl == "control")
        {
            mods.ctrl = true;
            continue;
        }
        if (tl == "shift")
        {
            mods.shift = true;
            continue;
        }
        if (tl == "alt" || tl == "option")
        {
            mods.alt = true;
            continue;
        }
        if (tl == "super" || tl == "meta" || tl == "win" || tl == "windows")
        {
            mods.super = true;
            continue;
        }
        if (tl == "cmd" || tl == "command")
        {
            mods.super = true;
            continue;
        }

        bool token_any_enter = false;
        bool implied_shift = false;
        const ImGuiKey k = KeyFromToken(tl, token_any_enter, implied_shift);
        if (k == ImGuiKey_None)
        {
            err = "unknown key token '" + tok + "'";
            return false;
        }
        if (key.has_value())
        {
            err = "multiple keys in chord '" + chord + "'";
            return false;
        }
        key = k;
        any_enter = token_any_enter;
        if (implied_shift)
            mods.shift = true;
    }

    if (!key.has_value())
    {
        err = "chord has no key";
        return false;
    }

    out.mods = mods;
    out.key = key.value();
    out.any_enter = any_enter;
    return true;
}

Platform RuntimePlatform()
{
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

KeyBindingsEngine::KeyBindingsEngine()
{
    SetDefaults(DefaultActions());
}

void KeyBindingsEngine::SetDefaults(std::vector<Action> defaults)
{
    defaults_ = std::move(defaults);
    runtime_dirty_ = true;
}

void KeyBindingsEngine::SetToolActions(std::vector<Action> tool_actions)
{
    tool_actions_ = std::move(tool_actions);
    runtime_dirty_ = true;

    // If we already have a live merged action list (e.g. Settings UI is open),
    // inject any newly-registered tool actions so they become editable immediately.
    if (loaded_ && !tool_actions_.empty())
    {
        std::unordered_set<std::string> ids;
        ids.reserve(actions_.size());
        for (const auto& a : actions_)
            ids.insert(a.id);
        for (const auto& ta : tool_actions_)
        {
            if (ids.insert(ta.id).second)
                actions_.push_back(ta);
        }
    }
}

std::vector<Action>& KeyBindingsEngine::ActionsMutable()
{
    return actions_;
}

std::vector<Action> KeyBindingsEngine::MergeDefaultsWithFile(const std::vector<Action>& defaults_plus_tools,
                                                             const std::vector<Action>& file_actions)
{
    std::vector<Action> merged = defaults_plus_tools;
    std::unordered_map<std::string, size_t> idx;
    idx.reserve(merged.size());
    for (size_t i = 0; i < merged.size(); ++i)
        idx[merged[i].id] = i;

    // Apply file actions:
    // - if action id exists, prefer file bindings (user edits)
    // - if unknown, append (preserve forward compatibility)
    for (const auto& fa : file_actions)
    {
        auto it = idx.find(fa.id);
        if (it == idx.end())
        {
            merged.push_back(fa);
            idx[merged.back().id] = merged.size() - 1;
            continue;
        }

        Action& dst = merged[it->second];
        // Keep default title/category/description if file omitted them.
        if (!fa.title.empty()) dst.title = fa.title;
        if (!fa.category.empty()) dst.category = fa.category;
        if (!fa.description.empty()) dst.description = fa.description;
        dst.bindings = fa.bindings;
    }

    return merged;
}

bool KeyBindingsEngine::LoadFromFile(const std::string& path, std::string& out_error)
{
    out_error.clear();
    path_ = path;

    // Build base action set: defaults + tool-registered actions.
    std::vector<Action> base = defaults_;
    {
        std::unordered_set<std::string> seen;
        seen.reserve(base.size() + tool_actions_.size());
        for (const auto& a : base) seen.insert(a.id);
        for (const auto& ta : tool_actions_)
        {
            if (seen.insert(ta.id).second)
                base.push_back(ta);
        }
    }

    std::ifstream f(path);
    if (!f)
    {
        // Missing file: fall back to defaults (and tool actions) and mark dirty so user can save.
        actions_ = std::move(base);
        loaded_ = true;
        dirty_ = true;
        last_error_ = std::string("Could not open '") + path + "'. Using defaults (not saved yet).";
        out_error = last_error_;
        runtime_dirty_ = true;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        actions_ = std::move(base);
        loaded_ = true;
        dirty_ = true;
        last_error_ = std::string("JSON parse error: ") + e.what();
        out_error = last_error_;
        runtime_dirty_ = true;
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

    std::vector<Action> file_actions;
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
        file_actions.push_back(std::move(a));
    }

    actions_ = MergeDefaultsWithFile(base, file_actions);
    loaded_ = true;
    dirty_ = false;
    last_error_.clear();
    runtime_dirty_ = true;
    return true;
}

bool KeyBindingsEngine::SaveToFile(const std::string& path, std::string& out_error) const
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

void KeyBindingsEngine::RebuildRuntime() const
{
    runtime_dirty_ = false;
    action_index_by_id_.clear();
    runtime_actions_.clear();
    runtime_actions_.reserve(actions_.size());
    action_index_by_id_.reserve(actions_.size());

    for (size_t i = 0; i < actions_.size(); ++i)
    {
        const Action& a = actions_[i];
        action_index_by_id_[a.id] = i;

        RuntimeAction ra;
        ra.id = a.id;
        ra.bindings.reserve(a.bindings.size());

        for (const auto& b : a.bindings)
        {
            RuntimeBinding rb;
            rb.enabled = b.enabled;
            rb.ctx = ContextFromString(b.context);
            rb.platform = PlatformFromString(b.platform);
            rb.chord_text = b.chord;

            std::string perr;
            if (!b.chord.empty() && ParseChordString(b.chord, rb.chord, perr))
            {
                ra.bindings.push_back(std::move(rb));
            }
            // If parsing fails, silently skip the binding at runtime; UI can still show/edit it.
        }

        runtime_actions_.push_back(std::move(ra));
    }
}

bool KeyBindingsEngine::ActionPressed(std::string_view action_id, const EvalContext& ctx) const
{
    if (runtime_dirty_)
        RebuildRuntime();

    const auto it = action_index_by_id_.find(std::string(action_id));
    if (it == action_index_by_id_.end())
        return false;
    const size_t idx = it->second;
    if (idx >= runtime_actions_.size())
        return false;

    const Platform plat = ctx.platform;
    const ImGuiIO& io = ImGui::GetIO();

    const RuntimeAction& ra = runtime_actions_[idx];
    for (const auto& b : ra.bindings)
    {
        if (!b.enabled)
            continue;
        if (!PlatformAllowed(b.platform, plat))
            continue;
        if (!ContextAllowed(b.ctx, ctx))
            continue;
        if (IsChordPressed(b.chord, io))
            return true;
    }
    return false;
}

Hotkeys KeyBindingsEngine::EvalCommonHotkeys(const EvalContext& ctx) const
{
    Hotkeys hk;
    hk.copy = ActionPressed("edit.copy", ctx);
    hk.cut = ActionPressed("edit.cut", ctx);
    hk.paste = ActionPressed("edit.paste", ctx);
    hk.select_all = ActionPressed("edit.select_all", ctx);
    hk.cancel = ActionPressed("selection.clear_or_cancel", ctx);
    hk.delete_selection = ActionPressed("selection.delete", ctx);
    return hk;
}

std::vector<Action> DefaultActions()
{
    // Seeded primarily from references/hotkeys.md "Common keybindings (cross-editor comparison)".
    // This is a curated set of common concepts; bindings include platform variants where known.
    // Note: This list intentionally matches the default `assets/key-bindings.json` shipped in-repo.
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

        // --- Editor ---
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

        // --- Character sets (insertion) ---
        // These map the active Character Set slots to keypresses.
        {
            .id="charset.insert.f1", .title="Insert Character Set Slot 1 (F1)", .category="Character Set",
            .description="Insert the glyph mapped to F1 in the active character set.",
            .bindings={ {.enabled=true, .chord="F1", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f2", .title="Insert Character Set Slot 2 (F2)", .category="Character Set",
            .description="Insert the glyph mapped to F2 in the active character set.",
            .bindings={ {.enabled=true, .chord="F2", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f3", .title="Insert Character Set Slot 3 (F3)", .category="Character Set",
            .description="Insert the glyph mapped to F3 in the active character set.",
            .bindings={ {.enabled=true, .chord="F3", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f4", .title="Insert Character Set Slot 4 (F4)", .category="Character Set",
            .description="Insert the glyph mapped to F4 in the active character set.",
            .bindings={ {.enabled=true, .chord="F4", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f5", .title="Insert Character Set Slot 5 (F5)", .category="Character Set",
            .description="Insert the glyph mapped to F5 in the active character set.",
            .bindings={ {.enabled=true, .chord="F5", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f6", .title="Insert Character Set Slot 6 (F6)", .category="Character Set",
            .description="Insert the glyph mapped to F6 in the active character set.",
            .bindings={ {.enabled=true, .chord="F6", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f7", .title="Insert Character Set Slot 7 (F7)", .category="Character Set",
            .description="Insert the glyph mapped to F7 in the active character set.",
            .bindings={ {.enabled=true, .chord="F7", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f8", .title="Insert Character Set Slot 8 (F8)", .category="Character Set",
            .description="Insert the glyph mapped to F8 in the active character set.",
            .bindings={ {.enabled=true, .chord="F8", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f9", .title="Insert Character Set Slot 9 (F9)", .category="Character Set",
            .description="Insert the glyph mapped to F9 in the active character set.",
            .bindings={ {.enabled=true, .chord="F9", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f10", .title="Insert Character Set Slot 10 (F10)", .category="Character Set",
            .description="Insert the glyph mapped to F10 in the active character set.",
            .bindings={ {.enabled=true, .chord="F10", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f11", .title="Insert Character Set Slot 11 (F11)", .category="Character Set",
            .description="Insert the glyph mapped to F11 in the active character set.",
            .bindings={ {.enabled=true, .chord="F11", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.f12", .title="Insert Character Set Slot 12 (F12)", .category="Character Set",
            .description="Insert the glyph mapped to F12 in the active character set.",
            .bindings={ {.enabled=true, .chord="F12", .context="editor", .platform="any"} }
        },
        // Alternate mapping: Ctrl+1..9,0 to slots 1..10 (matches current hardcoded behavior).
        {
            .id="charset.insert.ctrl_1", .title="Insert Character Set Slot 1 (Ctrl+1)", .category="Character Set",
            .description="Insert the glyph mapped to F1 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+1", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_2", .title="Insert Character Set Slot 2 (Ctrl+2)", .category="Character Set",
            .description="Insert the glyph mapped to F2 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+2", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_3", .title="Insert Character Set Slot 3 (Ctrl+3)", .category="Character Set",
            .description="Insert the glyph mapped to F3 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+3", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_4", .title="Insert Character Set Slot 4 (Ctrl+4)", .category="Character Set",
            .description="Insert the glyph mapped to F4 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+4", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_5", .title="Insert Character Set Slot 5 (Ctrl+5)", .category="Character Set",
            .description="Insert the glyph mapped to F5 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+5", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_6", .title="Insert Character Set Slot 6 (Ctrl+6)", .category="Character Set",
            .description="Insert the glyph mapped to F6 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+6", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_7", .title="Insert Character Set Slot 7 (Ctrl+7)", .category="Character Set",
            .description="Insert the glyph mapped to F7 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+7", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_8", .title="Insert Character Set Slot 8 (Ctrl+8)", .category="Character Set",
            .description="Insert the glyph mapped to F8 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+8", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_9", .title="Insert Character Set Slot 9 (Ctrl+9)", .category="Character Set",
            .description="Insert the glyph mapped to F9 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+9", .context="editor", .platform="any"} }
        },
        {
            .id="charset.insert.ctrl_0", .title="Insert Character Set Slot 10 (Ctrl+0)", .category="Character Set",
            .description="Insert the glyph mapped to F10 in the active character set.",
            .bindings={ {.enabled=true, .chord="Ctrl+0", .context="editor", .platform="any"} }
        },
    };
}

} // namespace kb


