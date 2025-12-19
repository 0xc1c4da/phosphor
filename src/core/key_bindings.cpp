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
    if (t == "." || t == "period" || t == "dot") return ImGuiKey_Period;
    if (t == "/" || t == "slash") return ImGuiKey_Slash;
    if (t == ";" || t == "semicolon") return ImGuiKey_Semicolon;
    if (t == "'" || t == "apostrophe" || t == "quote") return ImGuiKey_Apostrophe;
    if (t == "[" || t == "leftbracket" || t == "lbracket") return ImGuiKey_LeftBracket;
    if (t == "]" || t == "rightbracket" || t == "rbracket") return ImGuiKey_RightBracket;
    if (t == "\\" || t == "backslash") return ImGuiKey_Backslash;
    if (t == "`" || t == "grave" || t == "graveaccent") return ImGuiKey_GraveAccent;

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

static bool IsChordPressed(const ParsedChord& chord, const ImGuiIO& io, bool repeat)
{
    if (chord.key == ImGuiKey_None)
        return false;
    if (!ModExactMatch(chord.mods, io))
        return false;
    if (chord.any_enter)
        return ImGui::IsKeyPressed(ImGuiKey_Enter, repeat) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, repeat);
    return ImGui::IsKeyPressed(chord.key, repeat);
}

static bool KeyBindingToJson(const KeyBinding& b, json& out)
{
    out = json::object();
    out["enabled"] = b.enabled;
    out["chord"] = b.chord;
    out["context"] = b.context.empty() ? "global" : b.context;
    out["platform"] = b.platform.empty() ? "any" : b.platform;
    out["repeat"] = b.repeat;
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
    if (jb.contains("repeat") && jb["repeat"].is_boolean())
    {
        out.repeat = jb["repeat"].get<bool>();
        out.repeat_set = true;
    }
    else
    {
        out.repeat = false;
        out.repeat_set = false;
    }

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
    // Collapse consecutive empty tokens.
    // Example: "Ctrl++" initially yields ["Ctrl", "", ""] which previously caused a
    // "multiple keys" parse error. After collapsing -> ["Ctrl", ""] which maps to '+'.
    if (parts.size() >= 2)
    {
        std::vector<std::string> collapsed;
        collapsed.reserve(parts.size());
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (!collapsed.empty() && collapsed.back().empty() && parts[i].empty())
                continue;
            collapsed.push_back(std::move(parts[i]));
        }
        parts = std::move(collapsed);
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
        // Merge bindings with backward-compatible inheritance of new fields:
        // - Older key-bindings.json files won't include "repeat".
        // - If a file binding does not explicitly set repeat, inherit it from the matching default binding
        //   (by chord+context+platform), otherwise fall back to false.
        const std::vector<KeyBinding> defaults_bindings = dst.bindings;
        dst.bindings = fa.bindings;
        for (auto& b : dst.bindings)
        {
            if (b.repeat_set)
                continue;

            // Find a matching default binding to inherit from.
            bool inherited = false;
            for (const auto& db : defaults_bindings)
            {
                if (db.chord != b.chord) continue;
                if (db.context != b.context) continue;
                if (db.platform != b.platform) continue;
                b.repeat = db.repeat;
                b.repeat_set = true; // make it explicit going forward
                inherited = true;
                break;
            }
            if (!inherited)
            {
                b.repeat = false;
                // keep repeat_set=false to indicate "unspecified"; it may be written out as false later.
            }
        }
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
            rb.repeat = b.repeat;

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
        // Repeat behavior is per-binding. Most actions should remain single-shot on press;
        // Undo/Redo (and other similar actions) opt into repeat for "hold to repeat".
        if (IsChordPressed(b.chord, io, b.repeat))
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
        // --- Menu bar navigation (keyboard) ---
        // Note: Many apps use F10 to focus/open the menu bar, but Phosphor reserves F1..F12
        // (including F10) for Character Set insertion, so we use Alt-based bindings instead.
        {
            .id="menu.open.file", .title="Open File Menu", .category="Menu",
            .description="Open the File menu (keyboard navigation).",
            .bindings={ {.enabled=true, .chord="Alt+F", .context="global", .platform="any"} }
        },
        {
            .id="menu.open.edit", .title="Open Edit Menu", .category="Menu",
            .description="Open the Edit menu (keyboard navigation).",
            .bindings={ {.enabled=true, .chord="Alt+E", .context="global", .platform="any"} }
        },
        {
            .id="menu.open.window", .title="Open Window Menu", .category="Menu",
            .description="Open the Window menu (keyboard navigation).",
            .bindings={ {.enabled=true, .chord="Alt+W", .context="global", .platform="any"} }
        },

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
            .id="app.file.export_ansi", .title="Export ANSI…", .category="File",
            .description="Export the active canvas as an ANSI/text file.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Shift+E", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+Shift+E", .context="global", .platform="macos"},
            }
        },
        // TODO(support): Export as PNG/APNG/UTF-8 are common expectations (see references/hotkeys.md: Moebius).
        // Note: Moebius defaults conflict with our current Export ANSI binding; keep disabled until we reconcile.
        {
            .id="app.file.export_png", .title="Export PNG…", .category="File",
            .description="Export the active canvas as a PNG image.",
            .bindings={
                {.enabled=false, .chord="Ctrl+Shift+E", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Shift+E", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.export_apng", .title="Export Animated PNG…", .category="File",
            .description="Export animation as APNG. TODO(support).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Shift+A", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Shift+A", .context="global", .platform="macos"},
            }
        },
        {
            .id="app.file.export_utf8", .title="Export UTF-8…", .category="File",
            .description="Export as UTF-8 text.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Shift+U", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+Shift+U", .context="global", .platform="macos"},
            }
        },
        // TODO(support): SAUCE metadata editor is common in ANSI editors.
        {
            .id="app.file.edit_sauce", .title="Edit SAUCE…", .category="File",
            .description="Edit SAUCE metadata.",
            .bindings={
                {.enabled=true, .chord="Ctrl+I", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+I", .context="global", .platform="macos"},
                {.enabled=true, .chord="Ctrl+F11", .context="global", .platform="windows"},
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
                {.enabled=true, .chord="Ctrl+Z", .context="editor", .platform="any", .repeat=true, .repeat_set=true},
                {.enabled=true, .chord="Cmd+Z", .context="editor", .platform="macos", .repeat=true, .repeat_set=true},
            }
        },
        {
            .id="edit.redo", .title="Redo", .category="Edit",
            .description="Redo last undone operation.",
            .bindings={
                {.enabled=true, .chord="Ctrl+Shift+Z", .context="editor", .platform="any", .repeat=true, .repeat_set=true},
                {.enabled=true, .chord="Ctrl+Y", .context="editor", .platform="windows", .repeat=true, .repeat_set=true},
                {.enabled=true, .chord="Cmd+Shift+Z", .context="editor", .platform="macos", .repeat=true, .repeat_set=true},
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
        // TODO(wire): Optional clipboard mode used by some editors ("paste as selection").
        {
            .id="edit.paste_as_selection", .title="Paste As Selection", .category="Edit",
            .description="Paste clipboard into a floating selection layer. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+V", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+V", .context="editor", .platform="macos"},
            }
        },
        // TODO(wire): Swap foreground/background colors (common in Moebius).
        {
            .id="edit.swap_fg_bg", .title="Swap Foreground/Background", .category="Edit",
            .description="Swap the active foreground and background colors. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Shift+X", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+Shift+X", .context="editor", .platform="macos"},
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

        // TODO(wire): Selection operations (move/copy/fill/stamp/transform) are common, but need stronger gating
        // than our current EvalContext supports (these should only trigger when the selection tool is active).
        // Keep disabled-by-default until tools register these as tool actions or we add a 'tool' context.
        {
            .id="selection.op.move", .title="Move Block (Selection)", .category="Selection",
            .description="Move the active selection block. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="M", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.copy", .title="Copy Block (Selection)", .category="Selection",
            .description="Duplicate/copy the active selection block (not clipboard copy). TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="C", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.fill", .title="Fill Block (Selection)", .category="Selection",
            .description="Fill the selection with the current brush/attribute. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="F", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.erase", .title="Erase Block (Selection)", .category="Selection",
            .description="Erase the selection (alternate to Delete). TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="E", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.stamp", .title="Stamp Block (Selection)", .category="Selection",
            .description="Stamp/place the selection contents. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="S", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.place", .title="Place Block (Selection)", .category="Selection",
            .description="Commit/place the floating selection. TODO(wire; careful with Enter conflicts).",
            .bindings={ {.enabled=false, .chord="Enter", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.rotate_cw", .title="Rotate Selection (Clockwise)", .category="Selection",
            .description="Rotate selection clockwise. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="R", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.flip_x", .title="Flip Selection (Horizontal)", .category="Selection",
            .description="Flip selection horizontally. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="X", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.flip_y", .title="Flip Selection (Vertical)", .category="Selection",
            .description="Flip selection vertically. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="Y", .context="selection", .platform="any"} }
        },
        {
            .id="selection.op.center", .title="Center Selection", .category="Selection",
            .description="Center the selection on the canvas. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="=", .context="selection", .platform="any"} }
        },
        {
            .id="selection.paste.transparent_toggle", .title="Toggle Transparent Paste", .category="Selection",
            .description="Toggle transparent-paste mode for selection placement. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="T", .context="selection", .platform="any"} }
        },
        {
            .id="selection.paste.over_toggle", .title="Paste Mode: Over", .category="Selection",
            .description="Toggle 'Over' paste mode. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="O", .context="selection", .platform="any"} }
        },
        {
            .id="selection.paste.under_toggle", .title="Paste Mode: Under", .category="Selection",
            .description="Toggle 'Under' paste mode. TODO(wire; tool-gated).",
            .bindings={ {.enabled=false, .chord="U", .context="selection", .platform="any"} }
        },
        // TODO(wire): Crop to selection.
        {
            .id="selection.crop", .title="Crop to Selection", .category="Selection",
            .description="Crop canvas to the active selection bounds. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+K", .context="selection", .platform="any"},
                {.enabled=false, .chord="Cmd+K", .context="selection", .platform="macos"},
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
        // TODO(wire): Top/bottom of document (common in Icy Draw).
        {
            .id="nav.doc_top", .title="Top of Document", .category="Navigation",
            .description="Move caret to the top of the document. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Home", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+Home", .context="editor", .platform="macos"},
            }
        },
        {
            .id="nav.doc_bottom", .title="Bottom of Document", .category="Navigation",
            .description="Move caret to the bottom of the document. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+End", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+End", .context="editor", .platform="macos"},
            }
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
        // TODO(wire): Forward delete as an editor action (distinct from selection delete).
        {
            .id="editor.delete_forward", .title="Delete (Forward)", .category="Editor",
            .description="Delete character under caret. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Delete", .context="editor", .platform="any"} }
        },
        // TODO(wire): Tab / reverse-tab movement (common in Moebius/Icy Draw).
        {
            .id="editor.tab", .title="Tab", .category="Editor",
            .description="Move forward by tab stop / insert tab. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Tab", .context="editor", .platform="any"} }
        },
        {
            .id="editor.reverse_tab", .title="Reverse Tab", .category="Editor",
            .description="Move backward by tab stop. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Shift+Tab", .context="editor", .platform="any"} }
        },
        // TODO(wire): Overwrite/mirror modes (Moebius).
        {
            .id="editor.overwrite_mode_toggle", .title="Toggle Overwrite Mode", .category="Editor",
            .description="Toggle overwrite mode. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+O", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+O", .context="editor", .platform="macos"},
            }
        },
        {
            .id="editor.mirror_mode_toggle", .title="Toggle Mirror Mode", .category="Editor",
            .description="Toggle mirror mode. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+M", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+M", .context="editor", .platform="macos"},
            }
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
        // TODO(wire): Toggle iCE colors (Moebius).
        {
            .id="color.ice_toggle", .title="Toggle iCE Colors", .category="Color",
            .description="Toggle iCE colors. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+E", .context="editor", .platform="any"},
                {.enabled=false, .chord="Cmd+E", .context="editor", .platform="macos"},
            }
        },
        // TODO(wire): Direct color index selection is common, but conflicts with our current Ctrl+1..9 charset insert.
        // Keep disabled until we decide a non-conflicting mapping or introduce a mode.
        {
            .id="color.fg.set_0", .title="Set Foreground Color 0", .category="Color",
            .description="Set/toggle foreground color index 0. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+0", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_1", .title="Set Foreground Color 1", .category="Color",
            .description="Set/toggle foreground color index 1. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+1", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_2", .title="Set Foreground Color 2", .category="Color",
            .description="Set/toggle foreground color index 2. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+2", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_3", .title="Set Foreground Color 3", .category="Color",
            .description="Set/toggle foreground color index 3. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+3", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_4", .title="Set Foreground Color 4", .category="Color",
            .description="Set/toggle foreground color index 4. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+4", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_5", .title="Set Foreground Color 5", .category="Color",
            .description="Set/toggle foreground color index 5. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+5", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_6", .title="Set Foreground Color 6", .category="Color",
            .description="Set/toggle foreground color index 6. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+6", .context="editor", .platform="any"} }
        },
        {
            .id="color.fg.set_7", .title="Set Foreground Color 7", .category="Color",
            .description="Set/toggle foreground color index 7. TODO(wire; conflicts with charset).",
            .bindings={ {.enabled=false, .chord="Ctrl+7", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_0", .title="Set Background Color 0", .category="Color",
            .description="Set/toggle background color index 0. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+0", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_1", .title="Set Background Color 1", .category="Color",
            .description="Set/toggle background color index 1. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+1", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_2", .title="Set Background Color 2", .category="Color",
            .description="Set/toggle background color index 2. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+2", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_3", .title="Set Background Color 3", .category="Color",
            .description="Set/toggle background color index 3. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+3", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_4", .title="Set Background Color 4", .category="Color",
            .description="Set/toggle background color index 4. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+4", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_5", .title="Set Background Color 5", .category="Color",
            .description="Set/toggle background color index 5. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+5", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_6", .title="Set Background Color 6", .category="Color",
            .description="Set/toggle background color index 6. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+6", .context="editor", .platform="any"} }
        },
        {
            .id="color.bg.set_7", .title="Set Background Color 7", .category="Color",
            .description="Set/toggle background color index 7. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+7", .context="editor", .platform="any"} }
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
                // Note: Ctrl+0 conflicts with our Ctrl+0 Character Set insert mapping; use Ctrl+Alt+0 instead.
                {.enabled=true, .chord="Ctrl+Alt+0", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+0", .context="global", .platform="macos"},
            }
        },
        // TODO(wire): View/UI toggles common in Moebius/Icy Draw.
        {
            .id="view.fullscreen_toggle", .title="Toggle Fullscreen", .category="View",
            .description="Toggle fullscreen. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+F", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+F", .context="global", .platform="macos"},
                {.enabled=false, .chord="Alt+Enter", .context="global", .platform="any"},
            }
        },
        {
            .id="view.actual_size", .title="Actual Size", .category="View",
            .description="Reset view scale to actual size. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+0", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+0", .context="global", .platform="macos"},
            }
        },
        {
            .id="view.toggle_9px_font", .title="Toggle 9px Font", .category="View",
            .description="Toggle 9px font mode. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+F", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+F", .context="global", .platform="macos"},
            }
        },
        {
            .id="ui.toggle_status_bar", .title="Toggle Status Bar", .category="UI",
            .description="Show/hide status bar.",
            .bindings={
                {.enabled=true, .chord="Ctrl+/", .context="global", .platform="any"},
                {.enabled=true, .chord="Cmd+/", .context="global", .platform="macos"},
            }
        },
        {
            .id="ui.toggle_tool_bar", .title="Toggle Tool Bar", .category="UI",
            .description="Show/hide tool bar. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+T", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+T", .context="global", .platform="macos"},
            }
        },
        {
            .id="ui.toggle_preview", .title="Toggle Preview", .category="UI",
            .description="Show/hide preview window/pane. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+P", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+P", .context="global", .platform="macos"},
            }
        },
        {
            .id="view.toggle_scroll_with_cursor", .title="Toggle Scroll With Cursor", .category="View",
            .description="Toggle auto-scroll with caret/cursor. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+R", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+R", .context="global", .platform="macos"},
            }
        },
        // TODO(wire): Reference image support (Moebius/Icy Draw).
        {
            .id="view.reference_image.set", .title="Set Reference Image…", .category="View",
            .description="Load/set a reference image overlay. TODO(support/wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Shift+O", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Shift+O", .context="global", .platform="macos"},
            }
        },
        {
            .id="view.reference_image.toggle", .title="Toggle Reference Image", .category="View",
            .description="Toggle reference image overlay visibility. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Tab", .context="global", .platform="any"} }
        },
        // TODO(wire): Canvas scroll via keyboard (Moebius).
        {
            .id="view.scroll_up", .title="Scroll View Up", .category="View",
            .description="Scroll the canvas view up. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Alt+Up", .context="canvas", .platform="any"} }
        },
        {
            .id="view.scroll_down", .title="Scroll View Down", .category="View",
            .description="Scroll the canvas view down. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Alt+Down", .context="canvas", .platform="any"} }
        },
        {
            .id="view.scroll_left", .title="Scroll View Left", .category="View",
            .description="Scroll the canvas view left. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Alt+Left", .context="canvas", .platform="any"} }
        },
        {
            .id="view.scroll_right", .title="Scroll View Right", .category="View",
            .description="Scroll the canvas view right. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Alt+Right", .context="canvas", .platform="any"} }
        },
        // TODO(wire): Network/chat affordances (Moebius).
        {
            .id="net.connect", .title="Connect to Server…", .category="Network",
            .description="Connect to a collaboration/chat server. TODO(support/wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+Alt+S", .context="global", .platform="any"} }
        },
        {
            .id="ui.toggle_chat", .title="Toggle Chat Window", .category="UI",
            .description="Show/hide chat window. TODO(support/wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+[", .context="global", .platform="any"} }
        },

        // --- Canvas (row/col ops + justify/erase) ---
        // TODO(wire): Row/column editing operations are common in ANSI editors (Moebius/Icy Draw).
        {
            .id="canvas.row.insert", .title="Insert Row", .category="Canvas",
            .description="Insert a row at the caret. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Up", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.row.delete", .title="Delete Row", .category="Canvas",
            .description="Delete the row at the caret. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Down", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.col.insert", .title="Insert Column", .category="Canvas",
            .description="Insert a column at the caret. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Right", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.col.delete", .title="Delete Column", .category="Canvas",
            .description="Delete the column at the caret. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Left", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.justify_left", .title="Justify Line Left", .category="Canvas",
            .description="Left-justify the current line. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+L", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.justify_center", .title="Justify Line Center", .category="Canvas",
            .description="Center the current line. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+C", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.justify_right", .title="Justify Line Right", .category="Canvas",
            .description="Right-justify the current line. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+R", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_row", .title="Erase Row", .category="Canvas",
            .description="Erase the current row. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+E", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_row_to_start", .title="Erase Row to Start", .category="Canvas",
            .description="Erase from caret to start of row. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Home", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_row_to_end", .title="Erase Row to End", .category="Canvas",
            .description="Erase from caret to end of row. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+End", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_col", .title="Erase Column", .category="Canvas",
            .description="Erase the current column. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+Shift+E", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_col_to_start", .title="Erase Column to Start", .category="Canvas",
            .description="Erase from caret to top of column. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+PageUp", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.erase_col_to_end", .title="Erase Column to End", .category="Canvas",
            .description="Erase from caret to bottom of column. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Alt+PageDown", .context="editor", .platform="any"} }
        },
        {
            .id="canvas.set_size", .title="Set Canvas Size…", .category="Canvas",
            .description="Open canvas resize dialog. TODO(wire).",
            .bindings={
                {.enabled=false, .chord="Ctrl+Alt+C", .context="global", .platform="any"},
                {.enabled=false, .chord="Cmd+Alt+C", .context="global", .platform="macos"},
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

        // TODO(wire): Character-set navigation (Moebius) is useful when multiple sets exist.
        // This currently conflicts with some global UI toggles (Ctrl+/), so keep disabled by default.
        {
            .id="charset.prev_set", .title="Previous Character Set", .category="Character Set",
            .description="Select previous character set. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+,", .context="editor", .platform="any"} }
        },
        {
            .id="charset.next_set", .title="Next Character Set", .category="Character Set",
            .description="Select next character set. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+.", .context="editor", .platform="any"} }
        },
        {
            .id="charset.default_set", .title="Default Character Set", .category="Character Set",
            .description="Select default character set. TODO(wire).",
            .bindings={ {.enabled=false, .chord="Ctrl+/", .context="editor", .platform="any"} }
        },

        // TODO(wire): Tool mode switching (Moebius). These are intentionally disabled because
        // they conflict with typing unless we add an explicit "tool mode" or "command mode".
        {
            .id="tool.mode.keyboard", .title="Tool Mode: Keyboard", .category="Tools",
            .description="Switch to keyboard mode. TODO(wire; mode-gated).",
            .bindings={ {.enabled=false, .chord="K", .context="canvas", .platform="any"} }
        },
        {
            .id="tool.mode.brush", .title="Tool Mode: Brush", .category="Tools",
            .description="Switch to brush mode. TODO(wire; mode-gated).",
            .bindings={ {.enabled=false, .chord="B", .context="canvas", .platform="any"} }
        },
        {
            .id="tool.mode.shifter", .title="Tool Mode: Shifter", .category="Tools",
            .description="Switch to shifter mode. TODO(wire; mode-gated).",
            .bindings={ {.enabled=false, .chord="I", .context="canvas", .platform="any"} }
        },
        {
            .id="tool.mode.paintbucket", .title="Tool Mode: Paintbucket", .category="Tools",
            .description="Switch to paintbucket mode. TODO(wire; mode-gated).",
            .bindings={ {.enabled=false, .chord="P", .context="canvas", .platform="any"} }
        },
    };
}

} // namespace kb


