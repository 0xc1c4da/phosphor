#include "ui/command_palette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "imgui.h"
#include "imgui_internal.h"

#include "misc/cpp/imgui_stdlib.h"

#include "app/action_execute.h"
#include "app/action_route_execute.h"
#include "app/app_ui.h"

#include "core/canvas.h"
#include "core/colour_system.h"
#include "core/key_bindings.h"

#include "io/session/session_state.h"
#include "ui/tool_palette.h"

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

static int ScoreCandidate(std::string_view q_raw, std::string_view text_raw)
{
    if (q_raw.empty())
        return 0;

    std::string q = ToLower(std::string(q_raw));
    std::string t = ToLower(std::string(text_raw));

    // Exact prefix.
    if (t.rfind(q, 0) == 0)
        return 1000 + (int)q.size();

    // Word boundary / token prefix.
    for (size_t i = 0; i < t.size(); ++i)
    {
        const bool boundary = (i == 0) || (!std::isalnum((unsigned char)t[i - 1]) && std::isalnum((unsigned char)t[i]));
        if (boundary && t.compare(i, q.size(), q) == 0)
            return 700 + (int)q.size();
    }

    // Substring.
    if (t.find(q) != std::string::npos)
        return 400 + (int)q.size();

    // Simple subsequence fuzz.
    size_t qi = 0;
    int gaps = 0;
    for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti)
    {
        if (t[ti] == q[qi])
            ++qi;
        else
            ++gaps;
    }
    if (qi == q.size())
        return 200 + (int)q.size() - std::min(150, gaps);

    return -1;
}

static bool ParseHexNibble(char c, int& out)
{
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
    return false;
}

static std::optional<ImVec4> ParseHexColour(std::string_view s)
{
    // Accept #rgb, #rrggbb, #rrggbbaa (alpha ignored for now).
    if (s.size() < 2 || s[0] != '#')
        return std::nullopt;

    auto to_byte = [](int hi, int lo) { return (hi << 4) | lo; };

    if (s.size() == 4) // #rgb
    {
        int r, g, b;
        if (!ParseHexNibble(s[1], r) || !ParseHexNibble(s[2], g) || !ParseHexNibble(s[3], b))
            return std::nullopt;
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    if (s.size() == 7 || s.size() == 9) // #rrggbb or #rrggbbaa
    {
        int n[8] = {0,0,0,0,0,0,0,0};
        for (int i = 0; i < (int)s.size() - 1; ++i)
        {
            if (!ParseHexNibble(s[(size_t)i + 1], n[i]))
                return std::nullopt;
        }
        const int r = to_byte(n[0], n[1]);
        const int g = to_byte(n[2], n[3]);
        const int b = to_byte(n[4], n[5]);
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    return std::nullopt;
}

static std::uint32_t PackRgba32(const ImVec4& c)
{
    const int r = (int)std::lround(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
    const int g = (int)std::lround(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
    const int b = (int)std::lround(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    const int a = (int)std::lround(std::clamp(c.w, 0.0f, 1.0f) * 255.0f);
    return (std::uint32_t)((r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | ((a & 0xFF) << 24));
}

static ImVec4 UnpackRgba32(std::uint32_t rgba32)
{
    const float r = (float)((rgba32 >> 0) & 0xFF) / 255.0f;
    const float g = (float)((rgba32 >> 8) & 0xFF) / 255.0f;
    const float b = (float)((rgba32 >> 16) & 0xFF) / 255.0f;
    const float a = (float)((rgba32 >> 24) & 0xFF) / 255.0f;
    return ImVec4(r, g, b, a);
}

static void MruBump(std::vector<std::string>& v, const std::string& id, size_t cap)
{
    if (id.empty())
        return;
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
    v.insert(v.begin(), id);
    if (v.size() > cap)
        v.resize(cap);
}

static void MruBump(std::vector<std::uint32_t>& v, std::uint32_t rgba32, size_t cap)
{
    if (rgba32 == 0)
        return;
    v.erase(std::remove(v.begin(), v.end(), rgba32), v.end());
    v.insert(v.begin(), rgba32);
    if (v.size() > cap)
        v.resize(cap);
}

static bool IsColourModeQuery(std::string_view q_trimmed)
{
    if (q_trimmed.rfind("fg:", 0) == 0)
        q_trimmed = q_trimmed.substr(3);
    else if (q_trimmed.rfind("bg:", 0) == 0)
        q_trimmed = q_trimmed.substr(3);
    return !q_trimmed.empty() && q_trimmed.front() == '#';
}
} // namespace

void CommandPalette::Open(Mode mode)
{
    mode_ = mode;
    open_requested_ = true;
    is_open_ = true;
    focus_query_on_open_ = true;

    dbg_open_count_ += 1;
    dbg_opened_at_s_ = ImGui::GetTime();
    dbg_recorded_first_result_ = false;
    dbg_time_to_first_result_ms_ = 0.0;
    dbg_last_result_count_ = 0;

    query_.clear();
    selected_index_ = 0;
    all_items_.clear();
    results_.clear();
}

void CommandPalette::Close()
{
    is_open_ = false;
    open_requested_ = false;
    focus_query_on_open_ = false;
    dbg_close_count_ += 1;
    ImGui::CloseCurrentPopup();
}

void CommandPalette::rebuild_all_items(const RenderContext& ctx)
{
    all_items_.clear();

    // Actions
    for (const auto& a : ctx.keybinds.Actions())
    {
        Item it;
        it.kind = Item::Kind::Action;
        it.id = a.id;
        it.label = a.title.empty() ? a.id : a.title;
        it.detail = a.category;
        {
            // Best-effort shortcut display (spec polish): show the most relevant enabled binding.
            std::string_view preferred_ctx = "global";
            for (const auto& b : a.bindings)
            {
                if (!b.enabled)
                    continue;
                if (b.context == "selection") { preferred_ctx = "selection"; break; }
                if (b.context == "canvas") preferred_ctx = "canvas";
                else if (b.context == "editor" && preferred_ctx == "global") preferred_ctx = "editor";
            }
            const std::string sc = appui::ShortcutForAction(ctx.keybinds, it.id, preferred_ctx);
            if (!sc.empty())
            {
                if (!it.detail.empty())
                    it.detail += " · ";
                it.detail += sc;
            }
        }
        {
            std::string reason;
            app::RoutedActionExecContext rctx = {
                .host = ctx.action_exec,
                .tool_palette = ctx.tool_palette,
                .tool_engine = ctx.tool_engine,
                .compiled_tool_id = ctx.compiled_tool_id,
            };
            if (!app::CanExecuteRoutedActionId(it.id, rctx, reason))
            {
                it.disabled = true;
                it.disabled_reason = std::move(reason);
            }
        }
        all_items_.push_back(std::move(it));
    }

    // Tools
    for (const auto& t : ctx.tool_palette.GetTools())
    {
        if (t.id.empty())
            continue;
        Item it;
        it.kind = Item::Kind::Tool;
        it.id = t.id;
        it.label = t.label.empty() ? t.id : t.label;
        it.detail = "Tool";
        all_items_.push_back(std::move(it));
    }

    // Tool presets (active tool only; slots 1..9).
    if (const ToolSpec* at = ctx.tool_palette.GetActiveTool())
    {
        if (!at->id.empty())
        {
            for (int d = 1; d <= 9; ++d)
            {
                Item it;
                it.kind = Item::Kind::ToolPreset;
                it.id = "tool.preset." + at->id + "." + std::to_string(d);
                it.label = "Preset " + std::to_string(d);
                it.detail = at->label.empty() ? at->id : at->label;
                it.preset_digit = d;
                if (!ctx.apply_active_tool_preset_digit)
                {
                    it.disabled = true;
                    it.disabled_reason = "Preset apply not available";
                }
                all_items_.push_back(std::move(it));
            }
        }
    }

    // Windows
    for (const auto& w : ctx.windows)
    {
        if (!w.toggle)
            continue;
        // Focus item
        {
            Item it;
            it.kind = Item::Kind::Window;
            it.id = w.key + ".focus";
            it.label = w.label;
            it.detail = "Focus";
            all_items_.push_back(std::move(it));
        }
        // Toggle item
        {
            Item it;
            it.kind = Item::Kind::Window;
            it.id = w.key + ".toggle";
            it.label = w.label;
            it.detail = "Toggle";
            all_items_.push_back(std::move(it));
        }
    }
}

void CommandPalette::rebuild_results(const RenderContext& ctx)
{
    results_.clear();
    fg_lane_.clear();
    bg_lane_.clear();

    auto record_results = [&](int n) {
        dbg_last_result_count_ = n;
        if (!dbg_recorded_first_result_)
        {
            dbg_time_to_first_result_ms_ = (ImGui::GetTime() - dbg_opened_at_s_) * 1000.0;
            dbg_recorded_first_result_ = true;
        }
    };

    std::string q = Trim(query_);
    const bool in_colour_mode = (mode_ == Mode::Colour) || IsColourModeQuery(q);

    // Colour mode: build two lanes (FG/BG) from typed colour, MRU, and dominant colours.
    if (in_colour_mode)
    {
        bool explicit_target = false;
        bool target_bg = false;
        std::string_view colour_expr = std::string_view(q);
        if (q.rfind("fg:", 0) == 0) { explicit_target = true; target_bg = false; colour_expr = std::string_view(q).substr(3); }
        else if (q.rfind("bg:", 0) == 0) { explicit_target = true; target_bg = true; colour_expr = std::string_view(q).substr(3); }

        // 1) Typed colour preview (if valid).
        if (!colour_expr.empty() && colour_expr.front() == '#')
        {
            if (auto col = ParseHexColour(colour_expr))
            {
                Item it;
                it.kind = target_bg ? Item::Kind::ColourBg : Item::Kind::ColourFg;
                it.id = std::string(colour_expr);
                it.label = explicit_target ? "Typed" : "Typed (active lane)";
                it.detail = std::string(colour_expr);
                it.rgba32 = PackRgba32(*col);

                // If not explicitly targeted, place into the currently active lane.
                if (!explicit_target)
                {
                    it.kind = (active_colour_lane_ == 1) ? Item::Kind::ColourBg : Item::Kind::ColourFg;
                    if (it.kind == Item::Kind::ColourFg) fg_lane_.push_back(it);
                    else bg_lane_.push_back(it);
                }
                else
                {
                    if (it.kind == Item::Kind::ColourFg) fg_lane_.push_back(it);
                    else bg_lane_.push_back(it);
                }

                // 1b) Nearest palette index candidate (quantized), if we have an active canvas palette.
                if (ctx.active_canvas)
                {
                    auto& cs = phos::colour::GetColourSystem();
                    phos::colour::PaletteInstanceId pal = cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
                    if (auto id = cs.Palettes().Resolve(ctx.active_canvas->GetPaletteRef()))
                        pal = *id;
                    const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();

                    const ImVec4 c = *col;
                    const int r = (int)std::lround(c.x * 255.0f);
                    const int g = (int)std::lround(c.y * 255.0f);
                    const int b = (int)std::lround(c.z * 255.0f);
                    const int idx = (int)phos::colour::ColourOps::NearestIndexRgb(cs.Palettes(),
                                                                                 pal,
                                                                                 (std::uint8_t)std::clamp(r, 0, 255),
                                                                                 (std::uint8_t)std::clamp(g, 0, 255),
                                                                                 (std::uint8_t)std::clamp(b, 0, 255),
                                                                                 qp);
                    const std::uint32_t q32 = phos::colour::ColourOps::IndexToColour32(cs.Palettes(),
                                                                                      pal,
                                                                                      phos::colour::ColourIndex{(std::uint16_t)idx});
                    (void)idx; // detail only for now (UI will show it).
                    Item qi = it;
                    qi.label = "Quantized";
                    qi.detail = "nearest palette index " + std::to_string(idx);
                    qi.rgba32 = q32;
                    // Place quantized suggestion adjacent in same lane as the typed colour.
                    if (qi.kind == Item::Kind::ColourFg) fg_lane_.push_back(qi);
                    else bg_lane_.push_back(qi);
                }
            }
        }

        // 2) MRU colours (FG/BG).
        for (std::uint32_t c32 : ctx.session.command_palette.mru_fg_rgba32)
        {
            Item it;
            it.kind = Item::Kind::ColourFg;
            it.id = "mru_fg_" + std::to_string(c32);
            it.label = "MRU";
            it.detail.clear();
            it.rgba32 = c32;
            fg_lane_.push_back(std::move(it));
        }
        for (std::uint32_t c32 : ctx.session.command_palette.mru_bg_rgba32)
        {
            Item it;
            it.kind = Item::Kind::ColourBg;
            it.id = "mru_bg_" + std::to_string(c32);
            it.label = "MRU";
            it.detail.clear();
            it.rgba32 = c32;
            bg_lane_.push_back(std::move(it));
        }

        // 3) Dominant colours from active canvas (simple visible-region histogram on indices).
        if (ctx.active_canvas)
        {
            const auto& vs = ctx.active_canvas->GetLastViewState();
            const bool can_sample = vs.valid && vs.cell_w > 0.0f && vs.cell_h > 0.0f && vs.view_w > 0.0f && vs.view_h > 0.0f;
            const int cols = ctx.active_canvas->GetColumns();
            const int rows = ctx.active_canvas->GetRows();
            int c0 = 0, c1 = cols - 1, r0 = 0, r1 = rows - 1;
            if (can_sample)
            {
                c0 = (int)std::floor(vs.scroll_x / vs.cell_w);
                r0 = (int)std::floor(vs.scroll_y / vs.cell_h);
                c1 = (int)std::ceil((vs.scroll_x + vs.view_w) / vs.cell_w);
                r1 = (int)std::ceil((vs.scroll_y + vs.view_h) / vs.cell_h);
                c0 = std::clamp(c0, 0, cols - 1);
                r0 = std::clamp(r0, 0, rows - 1);
                c1 = std::clamp(c1, 0, cols - 1);
                r1 = std::clamp(r1, 0, rows - 1);
            }

            std::unordered_map<AnsiCanvas::ColourIndex16, int> fg_hist;
            std::unordered_map<AnsiCanvas::ColourIndex16, int> bg_hist;
            fg_hist.reserve(128);
            bg_hist.reserve(128);

            for (int rr = r0; rr <= r1; ++rr)
            {
                for (int cc = c0; cc <= c1; ++cc)
                {
                    char32_t cp = 0;
                    AnsiCanvas::ColourIndex16 fi = AnsiCanvas::kUnsetIndex16;
                    AnsiCanvas::ColourIndex16 bi = AnsiCanvas::kUnsetIndex16;
                    if (!ctx.active_canvas->GetCompositeCellPublicIndices(rr, cc, cp, fi, bi))
                        continue;
                    if (fi != AnsiCanvas::kUnsetIndex16) fg_hist[fi] += 1;
                    if (bi != AnsiCanvas::kUnsetIndex16) bg_hist[bi] += 1;
                }
            }

            auto emit_top = [&](const std::unordered_map<AnsiCanvas::ColourIndex16, int>& hist,
                                Item::Kind kind,
                                std::vector<Item>& out_lane) {
                struct Pair { AnsiCanvas::ColourIndex16 idx; int count; };
                std::vector<Pair> v;
                v.reserve(hist.size());
                for (const auto& kv : hist) v.push_back({kv.first, kv.second});
                std::sort(v.begin(), v.end(), [](const Pair& a, const Pair& b) { return a.count > b.count; });
                const int maxn = 16;
                for (int i = 0; i < (int)v.size() && i < maxn; ++i)
                {
                    const AnsiCanvas::Colour32 c32 = ctx.active_canvas->IndexToColour32Public(v[(size_t)i].idx);
                    if (c32 == 0)
                        continue;
                    Item it;
                    it.kind = kind;
                    it.id = "dom_" + std::to_string((int)kind) + "_" + std::to_string((int)v[(size_t)i].idx);
                    it.label = "Dominant";
                    it.detail = "idx " + std::to_string((int)v[(size_t)i].idx);
                    it.rgba32 = c32;
                    out_lane.push_back(std::move(it));
                }
            };

            emit_top(fg_hist, Item::Kind::ColourFg, fg_lane_);
            emit_top(bg_hist, Item::Kind::ColourBg, bg_lane_);
        }

        // Clamp lane indices.
        if (!fg_lane_.empty()) fg_lane_index_ = std::clamp(fg_lane_index_, 0, (int)fg_lane_.size() - 1);
        else fg_lane_index_ = 0;
        if (!bg_lane_.empty()) bg_lane_index_ = std::clamp(bg_lane_index_, 0, (int)bg_lane_.size() - 1);
        else bg_lane_index_ = 0;

        record_results((int)fg_lane_.size() + (int)bg_lane_.size());
        return;
    }

    // Empty query: curated MRU.
    if (q.empty())
    {
        // Repeat-last affordance.
        if (!ctx.session.command_palette.last_executed_item_id.empty())
        {
            for (const auto& it : all_items_)
            {
                if (it.id == ctx.session.command_palette.last_executed_item_id)
                {
                    Item rep = it;
                    rep.detail = "Repeat last";
                    results_.push_back(std::move(rep));
                    break;
                }
            }
        }

        auto add_by_id = [&](const std::vector<std::string>& ids) {
            for (const auto& id : ids)
            {
                for (const auto& it : all_items_)
                {
                    if (it.id == id)
                    {
                        results_.push_back(it);
                        break;
                    }
                }
            }
        };

        add_by_id(ctx.session.command_palette.mru_tool_ids);
        add_by_id(ctx.session.command_palette.mru_action_ids);
        add_by_id(ctx.session.command_palette.mru_window_keys);

        // MRU colours as list items (basic; colour mode provides the richer swatch UI).
        for (std::uint32_t c32 : ctx.session.command_palette.mru_fg_rgba32)
        {
            Item it;
            it.kind = Item::Kind::ColourFg;
            it.id = "colour.fg." + std::to_string(c32);
            it.label = "Set Foreground Colour";
            it.detail = "MRU";
            it.rgba32 = c32;
            results_.push_back(std::move(it));
        }
        for (std::uint32_t c32 : ctx.session.command_palette.mru_bg_rgba32)
        {
            Item it;
            it.kind = Item::Kind::ColourBg;
            it.id = "colour.bg." + std::to_string(c32);
            it.label = "Set Background Colour";
            it.detail = "MRU";
            it.rgba32 = c32;
            results_.push_back(std::move(it));
        }

        if ((int)results_.size() > 0)
            selected_index_ = std::clamp(selected_index_, 0, (int)results_.size() - 1);
        else
            selected_index_ = 0;
        record_results((int)results_.size());
        return;
    }

    // Search.
    struct Scored
    {
        int score = 0;
        Item item;
    };
    std::vector<Scored> scored;
    scored.reserve(all_items_.size());

    for (const auto& it : all_items_)
    {
        const int s0 = ScoreCandidate(q, it.label);
        const int s1 = ScoreCandidate(q, it.id);
        const int s = std::max(s0, s1);
        if (s < 0)
            continue;

        int bonus = 0;
        if (it.kind == Item::Kind::Action)
        {
            const auto& mru = ctx.session.command_palette.mru_action_ids;
            if (std::find(mru.begin(), mru.end(), it.id) != mru.end()) bonus += 25;
        }
        else if (it.kind == Item::Kind::Tool)
        {
            const auto& mru = ctx.session.command_palette.mru_tool_ids;
            if (std::find(mru.begin(), mru.end(), it.id) != mru.end()) bonus += 25;
        }
        else if (it.kind == Item::Kind::Window)
        {
            const auto& mru = ctx.session.command_palette.mru_window_keys;
            if (std::find(mru.begin(), mru.end(), it.id) != mru.end()) bonus += 15;
        }

        Scored sc;
        sc.score = s + bonus;
        sc.item = it;
        scored.push_back(std::move(sc));
    }

    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.item.label < b.item.label;
    });

    const size_t max_results = 64;
    for (size_t i = 0; i < scored.size() && results_.size() < max_results; ++i)
        results_.push_back(std::move(scored[i].item));

    if (!results_.empty())
        selected_index_ = std::clamp(selected_index_, 0, (int)results_.size() - 1);
    else
        selected_index_ = 0;

    record_results((int)results_.size());
}

void CommandPalette::Render(const RenderContext& ctx)
{
    if (!is_open_ && !open_requested_)
        return;

    const char* popup_name = "Command Palette###CommandPalette";

    if (open_requested_)
    {
        // Clear any previously active widget (notably the canvas' hidden InputText used for UTF-8 capture).
        // If we don't, it can remain ActiveId and prevent our query field from becoming active on open.
        ImGui::ClearActiveID();

        // Placement near caret (best effort), otherwise center.
        ImVec2 pos;
        bool have_pos = false;
        if (ctx.active_canvas)
            have_pos = ctx.active_canvas->GetCaretScreenPos(pos);
        if (!have_pos)
            pos = ImGui::GetMainViewport()->GetCenter();

        pos.x += 16.0f;
        pos.y += 16.0f;

        // Clamp to viewport work area.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 work_min = vp->WorkPos;
        const ImVec2 work_max = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
        pos.x = std::clamp(pos.x, work_min.x + 20.0f, work_max.x - 20.0f);
        pos.y = std::clamp(pos.y, work_min.y + 20.0f, work_max.y - 20.0f);

        ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_Appearing);
        ImGui::OpenPopup(popup_name);
        open_requested_ = false;

        rebuild_all_items(ctx);
        rebuild_results(ctx);
    }

    bool keep_open = true;
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings |
        0;

    // Ensure the popup itself gets focus when opened so the query box can reliably capture typing.
    if (focus_query_on_open_)
        ImGui::SetNextWindowFocus();

    if (ImGui::BeginPopupModal(popup_name, &keep_open, flags))
    {
        // While open, the palette must own keyboard focus (spec: "typed text goes to the palette").
        // In some ImGui focus edge-cases the previously focused canvas window can remain focused;
        // explicitly focus this popup window each frame to prevent leakage.
        ImGui::SetWindowFocus();

        if (!keep_open)
        {
            Close();
            if (ctx.request_focus_last_canvas) ctx.request_focus_last_canvas();
            ImGui::EndPopup();
            return;
        }

        // Close on Esc.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            Close();
            if (ctx.request_focus_last_canvas) ctx.request_focus_last_canvas();
            ImGui::EndPopup();
            return;
        }

        // Query box
        // Put the query first and force focus to it when the palette opens, so the canvas can't
        // keep capturing input behind us.
        ImGui::TextUnformatted(">");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const ImGuiID query_id = ImGui::GetID("##command_palette_query");
        // Focus the input on open (keyboard-driven; no click required).
        // Must be called BEFORE InputText, since it targets the *next* item.
        if (focus_query_on_open_ || ImGui::IsWindowAppearing())
        {
            // Stronger than SetNextWindowFocus() alone; makes sure this popup becomes the focused
            // window even if another widget was previously active.
            ImGui::SetWindowFocus();
            // Make the query box the active item for text editing.
            // This is the key difference between a "focused window" and an "active InputText":
            // without ActiveId, typed characters won't go into the text field.
            ImGui::ActivateItemByID(query_id);
            ImGui::SetKeyboardFocusHere();
        }
        const bool edited = ImGui::InputText("##command_palette_query", &query_);
        // Only clear the open-focus request once the query actually owns ActiveId.
        // If something else stole ActiveId on the opening frame (e.g. canvas hidden InputText),
        // we'll retry next frame until the query becomes active.
        if (ImGui::GetActiveID() == query_id)
            focus_query_on_open_ = false;
        if (edited)
            rebuild_results(ctx);

        // Optional debug/telemetry window (spec-recommended).
        if (ImGui::SmallButton(show_debug_window_ ? "Debug: ON" : "Debug: OFF"))
            show_debug_window_ = !show_debug_window_;

        const std::string qtrim = Trim(query_);
        const bool in_colour_mode = (mode_ == Mode::Colour) || IsColourModeQuery(qtrim);

        // Colour lanes UI (FG/BG swatches) + keyboard contract.
        if (in_colour_mode)
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Colours");
            ImGui::SameLine();
            ImGui::TextDisabled("(Tab: lane, Left/Right: FG, Alt+Left/Right: BG, Enter: apply, Shift+Enter: apply+stay)");

            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsKeyPressed(ImGuiKey_Tab))
                active_colour_lane_ = (active_colour_lane_ == 0) ? 1 : 0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            {
                if (io.KeyAlt)
                    bg_lane_index_ = std::max(0, bg_lane_index_ - 1);
                else
                    fg_lane_index_ = std::max(0, fg_lane_index_ - 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            {
                if (io.KeyAlt)
                    bg_lane_index_ = std::min((int)bg_lane_.size() - 1, bg_lane_index_ + 1);
                else
                    fg_lane_index_ = std::min((int)fg_lane_.size() - 1, fg_lane_index_ + 1);
            }

            auto render_lane = [&](const char* title, int lane, std::vector<Item>& lane_items, int& idx_ref) {
                ImGui::TextUnformatted(title);
                ImGui::SameLine();
                if (active_colour_lane_ == lane)
                    ImGui::TextColored(ImVec4(1,1,0.4f,1), "[active]");
                else
                    ImGui::TextDisabled("[inactive]");

                ImGui::BeginChild((std::string("##lane_") + title).c_str(), ImVec2(0, 56), false, ImGuiWindowFlags_NoScrollbar);
                for (int i = 0; i < (int)lane_items.size(); ++i)
                {
                    const Item& it = lane_items[(size_t)i];
                    const bool sel = (i == idx_ref);
                    ImGui::PushID(i);
                    if (i > 0) ImGui::SameLine();
                    ImGuiColorEditFlags cflags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder;
                    if (sel)
                        cflags &= ~ImGuiColorEditFlags_NoBorder;
                    const ImVec4 col = UnpackRgba32(it.rgba32);
                    const bool clicked = ImGui::ColorButton("##swatch", col, cflags, ImVec2(24, 24));
                    if (clicked)
                    {
                        idx_ref = i;
                        active_colour_lane_ = lane;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        if (!it.detail.empty())
                            ImGui::SetTooltip("%s", it.detail.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            };

            render_lane("FG", 0, fg_lane_, fg_lane_index_);
            render_lane("BG", 1, bg_lane_, bg_lane_index_);
        }

        // Keyboard navigation for the list.
        if (!in_colour_mode)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !results_.empty())
                selected_index_ = std::max(0, selected_index_ - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !results_.empty())
                selected_index_ = std::min((int)results_.size() - 1, selected_index_ + 1);
        }

        const bool pressed_enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        const bool pressed_shift_enter = pressed_enter && ImGui::GetIO().KeyShift;

        // Results list
        if (!in_colour_mode)
        {
            ImGui::BeginChild("##command_palette_results", ImVec2(0, 0), true);
            for (int i = 0; i < (int)results_.size(); ++i)
            {
                const Item& it = results_[(size_t)i];
                const bool selected = (i == selected_index_);

                // IMPORTANT: Many items can share identical visible text (especially MRU/recent),
                // which causes Dear ImGui "visible items with conflict Ids". Use stable IDs.
                ImGui::PushID(it.id.c_str());

                if (it.disabled)
                    ImGui::BeginDisabled(true);

                // Optional colour chip for colour items.
                if (it.kind == Item::Kind::ColourFg || it.kind == Item::Kind::ColourBg)
                {
                    const ImVec4 col = UnpackRgba32(it.rgba32);
                    ImGui::ColorButton("##chip",
                                       col,
                                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                       ImVec2(14, 14));
                    ImGui::SameLine();
                }

                std::string line = it.label;
                if (!it.detail.empty())
                {
                    line += "  ";
                    line += it.detail;
                }
                if (ImGui::Selectable(line.c_str(), selected))
                    selected_index_ = i;
                if (selected)
                {
                    dbg_last_selected_id_ = it.id;
                    dbg_last_selected_kind_.clear();
                    switch (it.kind)
                    {
                    case Item::Kind::Action: dbg_last_selected_kind_ = "action"; break;
                    case Item::Kind::Tool: dbg_last_selected_kind_ = "tool"; break;
                    case Item::Kind::ToolPreset: dbg_last_selected_kind_ = "tool_preset"; break;
                    case Item::Kind::Window: dbg_last_selected_kind_ = "window"; break;
                    case Item::Kind::ColourFg: dbg_last_selected_kind_ = "colour_fg"; break;
                    case Item::Kind::ColourBg: dbg_last_selected_kind_ = "colour_bg"; break;
                    default: break;
                    }
                }

                if (it.disabled && !it.disabled_reason.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", it.disabled_reason.c_str());

                if (it.disabled)
                    ImGui::EndDisabled();

                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        auto execute_item = [&](const Item& it, bool keep_palette_open) {
            if (it.disabled)
                return;

            bool executed = false;

            dbg_last_executed_id_ = it.id;
            dbg_last_executed_kind_.clear();
            switch (it.kind)
            {
            case Item::Kind::Action: dbg_last_executed_kind_ = "action"; break;
            case Item::Kind::Tool: dbg_last_executed_kind_ = "tool"; break;
            case Item::Kind::ToolPreset: dbg_last_executed_kind_ = "tool_preset"; break;
            case Item::Kind::Window: dbg_last_executed_kind_ = "window"; break;
            case Item::Kind::ColourFg: dbg_last_executed_kind_ = "colour_fg"; break;
            case Item::Kind::ColourBg: dbg_last_executed_kind_ = "colour_bg"; break;
            default: break;
            }

            if (it.kind == Item::Kind::Action)
            {
                app::RoutedActionExecContext rexec = {
                    .host = ctx.action_exec,
                    .tool_palette = ctx.tool_palette,
                    .tool_engine = ctx.tool_engine,
                    .compiled_tool_id = ctx.compiled_tool_id,
                };
                executed = app::ExecuteRoutedActionId(it.id, rexec);
                if (executed)
                    MruBump(ctx.session.command_palette.mru_action_ids, it.id, 32);
            }
            else if (it.kind == Item::Kind::Tool)
            {
                if (ctx.activate_tool_by_id)
                {
                    ctx.activate_tool_by_id(it.id);
                    executed = true;
                    MruBump(ctx.session.command_palette.mru_tool_ids, it.id, 16);
                }
            }
            else if (it.kind == Item::Kind::ToolPreset)
            {
                if (ctx.apply_active_tool_preset_digit && it.preset_digit >= 1 && it.preset_digit <= 9)
                {
                    executed = ctx.apply_active_tool_preset_digit(it.preset_digit);
                }
            }
            else if (it.kind == Item::Kind::Window)
            {
                for (const auto& w : ctx.windows)
                {
                    if (!w.toggle)
                        continue;
                    const std::string focus_id = w.key + ".focus";
                    const std::string toggle_id = w.key + ".toggle";
                    if (it.id == focus_id)
                    {
                        *w.toggle = true;
                        if (!w.imgui_focus.empty())
                            ImGui::SetWindowFocus(w.imgui_focus.c_str());
                        executed = true;
                        MruBump(ctx.session.command_palette.mru_window_keys, w.key, 16);
                        break;
                    }
                    if (it.id == toggle_id)
                    {
                        *w.toggle = !*w.toggle;
                        if (*w.toggle && !w.imgui_focus.empty())
                            ImGui::SetWindowFocus(w.imgui_focus.c_str());
                        executed = true;
                        MruBump(ctx.session.command_palette.mru_window_keys, w.key, 16);
                        break;
                    }
                }
            }
            else if (it.kind == Item::Kind::ColourFg || it.kind == Item::Kind::ColourBg)
            {
                const ImVec4 col = UnpackRgba32(it.rgba32);
                if (it.kind == Item::Kind::ColourBg)
                {
                    ctx.bg_colour = col;
                    MruBump(ctx.session.command_palette.mru_bg_rgba32, it.rgba32, 16);
                    ctx.session.command_palette.last_executed_item_id = "colour.bg." + std::to_string(it.rgba32);
                }
                else
                {
                    ctx.fg_colour = col;
                    MruBump(ctx.session.command_palette.mru_fg_rgba32, it.rgba32, 16);
                    ctx.session.command_palette.last_executed_item_id = "colour.fg." + std::to_string(it.rgba32);
                }
                executed = true;
            }

            dbg_last_execute_success_ = executed;
            if (executed)
            {
                if (ctx.session.command_palette.last_executed_item_id.empty() ||
                    (it.kind != Item::Kind::ColourFg && it.kind != Item::Kind::ColourBg))
                {
                    ctx.session.command_palette.last_executed_item_id = it.id;
                }
                if (!keep_palette_open)
                {
                    Close();
                    if (ctx.request_focus_last_canvas) ctx.request_focus_last_canvas();
                }
            }
        };

        if (pressed_enter)
        {
            if (in_colour_mode)
            {
                // Apply from active lane.
                const std::vector<Item>& lane = (active_colour_lane_ == 1) ? bg_lane_ : fg_lane_;
                const int idx = (active_colour_lane_ == 1) ? bg_lane_index_ : fg_lane_index_;
                if (!lane.empty() && idx >= 0 && idx < (int)lane.size())
                    execute_item(lane[(size_t)idx], /*keep_palette_open=*/pressed_shift_enter);
            }
            else
            {
                if (!results_.empty() && selected_index_ >= 0 && selected_index_ < (int)results_.size())
                    execute_item(results_[(size_t)selected_index_], /*keep_palette_open=*/pressed_shift_enter);
            }
        }

        ImGui::EndPopup();

        if (show_debug_window_)
        {
            if (ImGui::Begin("Command Palette Debug###CommandPaletteDebug", &show_debug_window_))
            {
                ImGui::Text("opens: %d  closes: %d", dbg_open_count_, dbg_close_count_);
                ImGui::Text("time-to-first-result: %.2f ms", dbg_time_to_first_result_ms_);
                ImGui::Text("last result count: %d", dbg_last_result_count_);
                ImGui::Separator();
                ImGui::Text("last selected: %s (%s)",
                            dbg_last_selected_id_.empty() ? "<none>" : dbg_last_selected_id_.c_str(),
                            dbg_last_selected_kind_.empty() ? "?" : dbg_last_selected_kind_.c_str());
                ImGui::Text("last executed: %s (%s)  success=%s",
                            dbg_last_executed_id_.empty() ? "<none>" : dbg_last_executed_id_.c_str(),
                            dbg_last_executed_kind_.empty() ? "?" : dbg_last_executed_kind_.c_str(),
                            dbg_last_execute_success_ ? "true" : "false");
            }
            ImGui::End();
        }
        return;
    }

    // Popup got closed externally.
    is_open_ = false;
    focus_query_on_open_ = false;
}


