#include "ui/command_palette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "imgui.h"
#include "imgui_internal.h"

#include "misc/cpp/imgui_stdlib.h"

#include "app/action_execute.h"
#include "app/action_route_execute.h"
#include "app/app_ui.h"

#include "core/canvas.h"
#include "core/colour_ops.h"
#include "core/colour_system.h"
#include "core/key_bindings.h"

#include "io/session/session_state.h"
#include "ui/tool_palette.h"

namespace
{
static inline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool IsToolActivateAction(std::string_view action_id, std::string_view& out_tool_id)
{
    const std::string_view prefix = "tool.activate.";
    if (!action_id.starts_with(prefix))
        return false;
    out_tool_id = action_id.substr(prefix.size());
    return !out_tool_id.empty();
}

static bool ToolExists(const ToolPalette& palette, std::string_view tool_id)
{
    for (const auto& t : palette.GetTools())
    {
        if (t.id == tool_id)
            return true;
    }
    return false;
}

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

static ImVec4 SnapRgbToActiveCanvasPalette(const ImVec4& rgb, const AnsiCanvas* active_canvas)
{
    auto& cs = phos::colour::GetColourSystem();
    phos::colour::PaletteInstanceId pal = cs.Palettes().Builtin(phos::colour::BuiltinPalette::Xterm256);
    if (active_canvas)
    {
        if (auto id = cs.Palettes().Resolve(active_canvas->GetPaletteRef()))
            pal = *id;
    }

    const int r = (int)std::lround(std::clamp(rgb.x, 0.0f, 1.0f) * 255.0f);
    const int g = (int)std::lround(std::clamp(rgb.y, 0.0f, 1.0f) * 255.0f);
    const int b = (int)std::lround(std::clamp(rgb.z, 0.0f, 1.0f) * 255.0f);

    const phos::colour::QuantizePolicy qp = phos::colour::DefaultQuantizePolicy();
    const std::uint32_t c32 = phos::colour::ColourOps::SnapRgbToColour32(cs.Palettes(),
                                                                         pal,
                                                                         (std::uint8_t)std::clamp(r, 0, 255),
                                                                         (std::uint8_t)std::clamp(g, 0, 255),
                                                                         (std::uint8_t)std::clamp(b, 0, 255),
                                                                         qp);
    ImVec4 out = UnpackRgba32(c32);
    out.w = rgb.w;
    return out;
}

static std::uint32_t SnapRgba32ToActiveCanvasPalette(std::uint32_t rgba32, const AnsiCanvas* active_canvas)
{
    if (rgba32 == 0)
        return 0;
    const ImVec4 raw = UnpackRgba32(rgba32);
    const ImVec4 snapped = SnapRgbToActiveCanvasPalette(raw, active_canvas);
    return PackRgba32(snapped);
}

struct DominantBounds
{
    int c0 = 0, c1 = 0, r0 = 0, r1 = 0;
    int cols = 0, rows = 0;
};

static DominantBounds ComputeDominantSamplingBounds(const AnsiCanvas& canvas)
{
    const auto& vs = canvas.GetLastViewState();
    const bool can_sample =
        vs.valid && vs.cell_w > 0.0f && vs.cell_h > 0.0f && vs.view_w > 0.0f && vs.view_h > 0.0f;

    const int cols = canvas.GetColumns();
    const int rows = canvas.GetRows();
    DominantBounds b;
    b.cols = cols;
    b.rows = rows;
    b.c0 = 0;
    b.r0 = 0;
    b.c1 = cols - 1;
    b.r1 = rows - 1;

    if (cols <= 0 || rows <= 0)
        return b;

    if (can_sample)
    {
        b.c0 = (int)std::floor(vs.scroll_x / vs.cell_w);
        b.r0 = (int)std::floor(vs.scroll_y / vs.cell_h);
        b.c1 = (int)std::ceil((vs.scroll_x + vs.view_w) / vs.cell_w);
        b.r1 = (int)std::ceil((vs.scroll_y + vs.view_h) / vs.cell_h);
        b.c0 = std::clamp(b.c0, 0, cols - 1);
        b.r0 = std::clamp(b.r0, 0, rows - 1);
        b.c1 = std::clamp(b.c1, 0, cols - 1);
        b.r1 = std::clamp(b.r1, 0, rows - 1);
    }
    return b;
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

static app::ActionExecContext MakePaletteHostExecContext(const CommandPalette::RenderContext& ctx)
{
    // When the command palette popup is open, Dear ImGui focus typically moves away from the canvas
    // window, and the app may consider there to be no "focused canvas" for this frame.
    //
    // For command palette actions, we want canvas-scoped actions (undo/redo/colour pick/etc) to apply to
    // the active/last canvas the user was editing, not to be disabled/ineffective just because the popup
    // owns focus.
    app::ActionExecContext host = ctx.action_exec;
    if (!host.focused_canvas && ctx.active_canvas)
    {
        host.focused_canvas = ctx.active_canvas;
        if (!host.focused_canvas_window)
            host.focused_canvas_window = host.active_canvas_window;
    }
    if (!host.active_canvas && ctx.active_canvas)
    {
        host.active_canvas = ctx.active_canvas;
        if (!host.active_canvas_window)
            host.active_canvas_window = host.focused_canvas_window;
    }
    return host;
}

static bool IsColourModeQuery(std::string_view q_trimmed)
{
    if (q_trimmed.rfind("fg:", 0) == 0)
        q_trimmed = q_trimmed.substr(3);
    else if (q_trimmed.rfind("bg:", 0) == 0)
        q_trimmed = q_trimmed.substr(3);
    return !q_trimmed.empty() && q_trimmed.front() == '#';
}

static void DrawPaletteStyleSelectionMarkers(bool mark_foreground, bool mark_background)
{
    // Visual selection indicators: FG = outer outline + top-left corner triangle,
    // BG = inner outline + bottom-right corner triangle.
    if (!mark_foreground && !mark_background)
        return;

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImU32 shadow = IM_COL32(0, 0, 0, 170);
    // Match `src/ui/colour_palette.cpp` selection colours:
    // - Foreground marker: white
    // - Background marker: black
    const ImU32 fg_col = IM_COL32(255, 255, 255, 255);
    const ImU32 bg_col = IM_COL32(0, 0, 0, 255);

    if (mark_foreground)
    {
        const float t = 2.0f;
        dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f),
                    ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                    shadow, rounding, 0, t + 1.0f);
        dl->AddRect(p0, p1, fg_col, rounding, 0, t);

        const float ts = ClampF((p1.x - p0.x) * 0.45f, 8.0f, 18.0f);
        const ImVec2 a(p0.x + 1.0f, p0.y + 1.0f);
        const ImVec2 b(p0.x + 1.0f + ts, p0.y + 1.0f);
        const ImVec2 c(p0.x + 1.0f, p0.y + 1.0f + ts);
        dl->AddTriangleFilled(a, b, c, fg_col);
    }

    if (mark_background)
    {
        const float inset = 3.5f;
        const ImVec2 q0(p0.x + inset, p0.y + inset);
        const ImVec2 q1(p1.x - inset, p1.y - inset);
        if (q1.x > q0.x + 2.0f && q1.y > q0.y + 2.0f)
        {
            const float t = 2.0f;
            dl->AddRect(ImVec2(q0.x - 1.0f, q0.y - 1.0f),
                        ImVec2(q1.x + 1.0f, q1.y + 1.0f),
                        shadow, rounding * 0.75f, 0, t + 1.0f);
            dl->AddRect(q0, q1, bg_col, rounding * 0.75f, 0, t);

            const float ts = ClampF((p1.x - p0.x) * 0.45f, 8.0f, 18.0f);
            const ImVec2 a(p1.x - 1.0f, p1.y - 1.0f);
            const ImVec2 b(p1.x - 1.0f - ts, p1.y - 1.0f);
            const ImVec2 c(p1.x - 1.0f, p1.y - 1.0f - ts);
            dl->AddTriangleFilled(a, b, c, bg_col);
            dl->AddTriangle(a, b, c, shadow, 1.0f);
        }
    }
}
} // namespace

void CommandPalette::Open(Mode mode)
{
    mode_ = mode;
    open_requested_ = true;
    is_open_ = true;
    focus_query_on_open_ = true;
    colour_lane_interacted_ = false;

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
            // Special-case: tool activation actions (`tool.activate.<tool_id>`) are executed by the
            // app/tool switching layer (RunFrame) and aren't "tool-claimed actions" routed through tools.
            // In the palette, treat them as runnable iff we have an activation callback and the tool exists.
            std::string_view tool_id;
            if (IsToolActivateAction(it.id, tool_id))
            {
                if (!ctx.activate_tool_by_id)
                {
                    it.disabled = true;
                    it.disabled_reason = "Tool activation not available";
                }
                else if (!ToolExists(ctx.tool_palette, tool_id))
                {
                    it.disabled = true;
                    it.disabled_reason = "Unknown tool";
                }
            }
            else
            {
                std::string reason;
                const app::ActionExecContext host = MakePaletteHostExecContext(ctx);
                app::RoutedActionExecContext rctx = {
                    .host = host,
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

    std::string q = Trim(query_);
    const bool in_colour_mode = (mode_ == Mode::Colour) || IsColourModeQuery(q);

    // Always build FG/BG lanes (so strips are visible even without typing '#').
    {
        // 0) Always include current FG/BG as the first swatch so Enter-to-apply never surprises you.
        {
            Item it;
            it.kind = Item::Kind::ColourFg;
            it.id = "colour.fg.current";
            it.label = "Current";
            it.detail.clear();
            it.rgba32 = PackRgba32(ctx.fg_colour);
            fg_lane_.push_back(std::move(it));
        }
        {
            Item it;
            it.kind = Item::Kind::ColourBg;
            it.id = "colour.bg.current";
            it.label = "Current";
            it.detail.clear();
            it.rgba32 = PackRgba32(ctx.bg_colour);
            bg_lane_.push_back(std::move(it));
        }
    }

    // Add typed colour preview (snapped to the active canvas palette, so "what you see" matches tools).
    {
        bool explicit_target = false;
        bool target_bg = false;
        std::string_view colour_expr = std::string_view(q);
        if (q.rfind("fg:", 0) == 0) { explicit_target = true; target_bg = false; colour_expr = std::string_view(q).substr(3); }
        else if (q.rfind("bg:", 0) == 0) { explicit_target = true; target_bg = true; colour_expr = std::string_view(q).substr(3); }

        int target_fb = 0; // 0=FG, 1=BG
        if (explicit_target)
            target_fb = target_bg ? 1 : 0;
        else if (ctx.active_fb)
            target_fb = std::clamp(*ctx.active_fb, 0, 1);

        // 1) Typed colour preview (if valid).
        if (!colour_expr.empty() && colour_expr.front() == '#')
        {
            if (auto col = ParseHexColour(colour_expr))
            {
                Item it;
                it.kind = (target_fb == 1) ? Item::Kind::ColourBg : Item::Kind::ColourFg;
                it.id = std::string(colour_expr);
                it.label = "Typed";
                it.detail = std::string(colour_expr);
                // Snap to palette immediately so the swatch reflects the effective tool/canvas colour.
                it.rgba32 = PackRgba32(SnapRgbToActiveCanvasPalette(*col, ctx.active_canvas));

                // Semantics:
                if (it.kind == Item::Kind::ColourFg)
                {
                    fg_lane_.push_back(it);
                    // Select the typed swatch for FG by default when a colour is typed.
                    fg_lane_index_ = (int)fg_lane_.size() - 1;
                }
                else
                {
                    bg_lane_.push_back(it);
                    bg_lane_index_ = (int)bg_lane_.size() - 1;
                }
            }
        }
    }

    // 2) MRU colours (FG/BG).
    // NOTE: We intentionally do NOT dedupe against Current/Typed, but we DO treat MRU+Dominant as a
    // single suggestion set (deduped after quantization) to avoid repeated swatches.
    std::unordered_set<std::uint32_t> fg_suggest_seen;
    std::unordered_set<std::uint32_t> bg_suggest_seen;
    fg_suggest_seen.reserve(ctx.session.command_palette.mru_fg_rgba32.size() + 32);
    bg_suggest_seen.reserve(ctx.session.command_palette.mru_bg_rgba32.size() + 32);

    for (std::uint32_t c32 : ctx.session.command_palette.mru_fg_rgba32)
    {
        const std::uint32_t snapped = SnapRgba32ToActiveCanvasPalette(c32, ctx.active_canvas);
        if (snapped == 0 || fg_suggest_seen.count(snapped) != 0)
            continue;
        fg_suggest_seen.insert(snapped);
        Item it;
        it.kind = Item::Kind::ColourFg;
        it.id = "mru_fg_" + std::to_string(snapped);
        it.label = "MRU";
        it.detail.clear();
        it.rgba32 = snapped;
        fg_lane_.push_back(std::move(it));
    }
    for (std::uint32_t c32 : ctx.session.command_palette.mru_bg_rgba32)
    {
        const std::uint32_t snapped = SnapRgba32ToActiveCanvasPalette(c32, ctx.active_canvas);
        if (snapped == 0 || bg_suggest_seen.count(snapped) != 0)
            continue;
        bg_suggest_seen.insert(snapped);
        Item it;
        it.kind = Item::Kind::ColourBg;
        it.id = "mru_bg_" + std::to_string(snapped);
        it.label = "MRU";
        it.detail.clear();
        it.rgba32 = snapped;
        bg_lane_.push_back(std::move(it));
    }

    // 3) Dominant colours from active canvas (simple visible-region histogram on indices).
    if (ctx.active_canvas)
    {
        const AnsiCanvas& canvas = *ctx.active_canvas;
        const DominantBounds b = ComputeDominantSamplingBounds(canvas);
        const std::uint64_t rev = canvas.GetContentRevision();

        const bool cache_ok =
            dominant_cache_.canvas == &canvas &&
            dominant_cache_.content_rev == rev &&
            dominant_cache_.c0 == b.c0 && dominant_cache_.c1 == b.c1 &&
            dominant_cache_.r0 == b.r0 && dominant_cache_.r1 == b.r1 &&
            dominant_cache_.cols == b.cols && dominant_cache_.rows == b.rows;

        if (!cache_ok)
        {
            dominant_cache_.canvas = &canvas;
            dominant_cache_.content_rev = rev;
            dominant_cache_.c0 = b.c0;
            dominant_cache_.c1 = b.c1;
            dominant_cache_.r0 = b.r0;
            dominant_cache_.r1 = b.r1;
            dominant_cache_.cols = b.cols;
            dominant_cache_.rows = b.rows;
            dominant_cache_.fg.clear();
            dominant_cache_.bg.clear();

            std::unordered_map<AnsiCanvas::ColourIndex16, int> fg_hist;
            std::unordered_map<AnsiCanvas::ColourIndex16, int> bg_hist;
            fg_hist.reserve(128);
            bg_hist.reserve(128);

            for (int rr = b.r0; rr <= b.r1; ++rr)
            {
                for (int cc = b.c0; cc <= b.c1; ++cc)
                {
                    char32_t cp = 0;
                    AnsiCanvas::ColourIndex16 fi = AnsiCanvas::kUnsetIndex16;
                    AnsiCanvas::ColourIndex16 bi = AnsiCanvas::kUnsetIndex16;
                    if (!canvas.GetCompositeCellPublicIndices(rr, cc, cp, fi, bi))
                        continue;
                    if (fi != AnsiCanvas::kUnsetIndex16) fg_hist[fi] += 1;
                    if (bi != AnsiCanvas::kUnsetIndex16) bg_hist[bi] += 1;
                }
            }

            auto build_cache = [&](const std::unordered_map<AnsiCanvas::ColourIndex16, int>& hist,
                                   std::vector<DominantCacheEntry>& out) {
                struct Pair { AnsiCanvas::ColourIndex16 idx; int count; };
                std::vector<Pair> v;
                v.reserve(hist.size());
                for (const auto& kv : hist) v.push_back({kv.first, kv.second});
                std::sort(v.begin(), v.end(), [](const Pair& a, const Pair& b) { return a.count > b.count; });
                const int maxn = 16;
                out.reserve((size_t)maxn);
                for (int i = 0; i < (int)v.size() && i < maxn; ++i)
                {
                    const AnsiCanvas::Colour32 c32 = canvas.IndexToColour32Public(v[(size_t)i].idx);
                    if (c32 == 0)
                        continue;
                    DominantCacheEntry e;
                    e.idx = (int)v[(size_t)i].idx;
                    e.rgba32 = (std::uint32_t)c32;
                    out.push_back(e);
                }
            };

            build_cache(fg_hist, dominant_cache_.fg);
            build_cache(bg_hist, dominant_cache_.bg);
        }

        auto emit_cached = [&](const std::vector<DominantCacheEntry>& cached,
                               Item::Kind kind,
                               std::vector<Item>& out_lane,
                               std::unordered_set<std::uint32_t>& suggest_seen) {
            for (const DominantCacheEntry& e : cached)
            {
                if (e.rgba32 == 0)
                    continue;
                if (suggest_seen.count(e.rgba32) != 0)
                    continue;
                suggest_seen.insert(e.rgba32);
                Item it;
                it.kind = kind;
                it.id = "dom_" + std::to_string((int)kind) + "_" + std::to_string(e.idx);
                it.label = "Dominant";
                it.detail = "idx " + std::to_string(e.idx);
                it.rgba32 = e.rgba32;
                out_lane.push_back(std::move(it));
            }
        };

        emit_cached(dominant_cache_.fg, Item::Kind::ColourFg, fg_lane_, fg_suggest_seen);
        emit_cached(dominant_cache_.bg, Item::Kind::ColourBg, bg_lane_, bg_suggest_seen);
    }

    // Clamp lane indices.
    if (!fg_lane_.empty()) fg_lane_index_ = std::clamp(fg_lane_index_, 0, (int)fg_lane_.size() - 1);
    else fg_lane_index_ = 0;
    if (!bg_lane_.empty()) bg_lane_index_ = std::clamp(bg_lane_index_, 0, (int)bg_lane_.size() - 1);
    else bg_lane_index_ = 0;

    // If the query is explicitly a colour query, we don't build the command results list.
    if (in_colour_mode)
    {
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
        {
            std::unordered_set<std::uint32_t> seen;
            seen.reserve(ctx.session.command_palette.mru_fg_rgba32.size());
            for (std::uint32_t c32 : ctx.session.command_palette.mru_fg_rgba32)
            {
                const std::uint32_t snapped = SnapRgba32ToActiveCanvasPalette(c32, ctx.active_canvas);
                if (snapped == 0 || seen.count(snapped) != 0)
                    continue;
                seen.insert(snapped);
                Item it;
                it.kind = Item::Kind::ColourFg;
                it.id = "colour.fg." + std::to_string(snapped);
                it.label = "Set Foreground Colour";
                it.detail = "MRU";
                it.rgba32 = snapped;
                results_.push_back(std::move(it));
            }
        }
        {
            std::unordered_set<std::uint32_t> seen;
            seen.reserve(ctx.session.command_palette.mru_bg_rgba32.size());
            for (std::uint32_t c32 : ctx.session.command_palette.mru_bg_rgba32)
            {
                const std::uint32_t snapped = SnapRgba32ToActiveCanvasPalette(c32, ctx.active_canvas);
                if (snapped == 0 || seen.count(snapped) != 0)
                    continue;
                seen.insert(snapped);
                Item it;
                it.kind = Item::Kind::ColourBg;
                it.id = "colour.bg." + std::to_string(snapped);
                it.label = "Set Background Colour";
                it.detail = "MRU";
                it.rgba32 = snapped;
                results_.push_back(std::move(it));
            }
        }

        if ((int)results_.size() > 0)
            selected_index_ = std::clamp(selected_index_, 0, (int)results_.size() - 1);
        else
            selected_index_ = 0;
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

        // Build initial item lists BEFORE sizing the window, so we can auto-size to fit the longer
        // of the FG/BG colour lanes (no manual resize required to see all swatches).
        rebuild_all_items(ctx);
        rebuild_results(ctx);

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

        // Auto-width to fit the longest colour lane.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float swatch_w = 24.0f;
        const float swatch_spacing_x = style.ItemSpacing.x;
        const int max_lane_n = std::max((int)fg_lane_.size(), (int)bg_lane_.size());
        const float lane_w = (max_lane_n <= 0)
            ? 0.0f
            : (max_lane_n * swatch_w + (max_lane_n - 1) * swatch_spacing_x);
        const float label_w = ImGui::CalcTextSize("FG").x;
        const float content_w = label_w + style.ItemSpacing.x + lane_w;
        float win_w = content_w + style.WindowPadding.x * 2.0f + 10.0f;
        // Clamp to viewport; if the lane is wider than the monitor, let it clip rather than
        // forcing off-screen placement.
        const float max_w = vp->WorkSize.x - 40.0f;
        win_w = std::clamp(win_w, 420.0f, std::max(420.0f, max_w));

        ImGui::SetNextWindowSize(ImVec2(win_w, 360), ImGuiCond_Appearing);
        ImGui::OpenPopup(popup_name);
        open_requested_ = false;
    }

    bool keep_open = true;
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings |
        // Disable Dear ImGui's native keyboard navigation within the palette. We implement our own
        // focused behavior (query always active; arrows/Tab have palette-specific semantics).
        ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus |
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
            // Prevent Esc from also reaching canvas/tool key handlers this frame.
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetCurrentWindow()->ID, ImGuiInputFlags_LockThisFrame);
            Close();
            if (ctx.request_focus_last_canvas) ctx.request_focus_last_canvas();
            ImGui::EndPopup();
            return;
        }

        const std::string qtrim = Trim(query_);
        const bool in_colour_mode = (mode_ == Mode::Colour) || IsColourModeQuery(qtrim);

        // Colour lanes UI (FG/BG swatches) are always visible and live directly above the query box.
        ImGuiStyle& style = ImGui::GetStyle();
        // Tight layout: no extra vertical gaps between FG/BG rows, and no extra spacing after BG.
        // (We keep this active through the query line so BG sits flush to the input.)
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
        {
            const ImGuiIO& io = ImGui::GetIO();
            const ImGuiID key_owner = ImGui::GetCurrentWindow()->ID;

            // Tab toggles the shared FG/BG focus (same semantics as the colour picker).
            // This affects where '#...' (without fg:/bg:) targets.
            if (ImGui::IsKeyPressed(ImGuiKey_Tab) && ctx.active_fb)
            {
                // Prevent Tab from participating in ImGui's focus navigation.
                ImGui::SetKeyOwner(ImGuiKey_Tab, key_owner, ImGuiInputFlags_LockThisFrame);
                *ctx.active_fb = 1 - std::clamp(*ctx.active_fb, 0, 1);
                rebuild_results(ctx);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            {
                // Arrow keys are palette-owned (do not move InputText cursor or ImGui nav focus).
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow, key_owner, ImGuiInputFlags_LockThisFrame);
                if (io.KeyAlt)
                {
                    if (!bg_lane_.empty())
                        bg_lane_index_ = std::max(0, bg_lane_index_ - 1);
                    if (ctx.active_fb) *ctx.active_fb = 1;
                }
                else
                {
                    if (!fg_lane_.empty())
                        fg_lane_index_ = std::max(0, fg_lane_index_ - 1);
                    if (ctx.active_fb) *ctx.active_fb = 0;
                }
                colour_lane_interacted_ = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            {
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, key_owner, ImGuiInputFlags_LockThisFrame);
                if (io.KeyAlt)
                {
                    if (!bg_lane_.empty())
                        bg_lane_index_ = std::min((int)bg_lane_.size() - 1, bg_lane_index_ + 1);
                    if (ctx.active_fb) *ctx.active_fb = 1;
                }
                else
                {
                    if (!fg_lane_.empty())
                        fg_lane_index_ = std::min((int)fg_lane_.size() - 1, fg_lane_index_ + 1);
                    if (ctx.active_fb) *ctx.active_fb = 0;
                }
                colour_lane_interacted_ = true;
            }

            // Up/Down: results list navigation (when present). Handle BEFORE InputText so the arrows
            // don't also move the text cursor / interact with ImGui nav.
            if (!in_colour_mode)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !results_.empty())
                {
                    ImGui::SetKeyOwner(ImGuiKey_UpArrow, key_owner, ImGuiInputFlags_LockThisFrame);
                    selected_index_ = std::max(0, selected_index_ - 1);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !results_.empty())
                {
                    ImGui::SetKeyOwner(ImGuiKey_DownArrow, key_owner, ImGuiInputFlags_LockThisFrame);
                    selected_index_ = std::min((int)results_.size() - 1, selected_index_ + 1);
                }
            }

            auto render_lane = [&](const char* title, std::vector<Item>& lane_items, int& idx_ref) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(title);
                ImGui::SameLine();

                // Child window defaults add padding; push a tighter child style so the strip doesn't
                // look like it has blank "gutter" space above/below.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));

                ImGui::BeginChild((std::string("##lane_") + title).c_str(),
                                  ImVec2(0, 28),
                                  false,
                                  ImGuiWindowFlags_NoScrollbar);
                for (int i = 0; i < (int)lane_items.size(); ++i)
                {
                    const Item& it = lane_items[(size_t)i];
                    const bool sel = (i == idx_ref);
                    ImGui::PushID(i);
                    if (i > 0) ImGui::SameLine();
                    ImGuiColorEditFlags cflags =
                        ImGuiColorEditFlags_NoAlpha |
                        ImGuiColorEditFlags_NoPicker |
                        ImGuiColorEditFlags_NoTooltip |
                        ImGuiColorEditFlags_NoDragDrop |
                        ImGuiColorEditFlags_NoBorder;
                    const ImVec4 col = UnpackRgba32(it.rgba32);
                    const bool clicked = ImGui::ColorButton("##swatch", col, cflags, ImVec2(24, 24));
                    if (clicked)
                    {
                        idx_ref = i;
                        colour_lane_interacted_ = true;
                        if (ctx.active_fb)
                            *ctx.active_fb = (title[0] == 'B') ? 1 : 0;
                    }
                    if (sel)
                    {
                        // Match selection styling in `src/ui/colour_palette.cpp`.
                        const bool mark_fg = (title[0] == 'F'); // "FG"
                        const bool mark_bg = (title[0] == 'B'); // "BG"
                        DrawPaletteStyleSelectionMarkers(mark_fg, mark_bg);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        if (!it.detail.empty())
                            ImGui::SetTooltip("%s", it.detail.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                ImGui::PopStyleVar(2);
            };

            render_lane("FG", fg_lane_, fg_lane_index_);
            render_lane("BG", bg_lane_, bg_lane_index_);
        }

        // Query box (directly below the colour strips).
        ImGui::TextUnformatted(">");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const ImGuiID query_id = ImGui::GetID("##command_palette_query");
        // Focus the input on open (keyboard-driven; no click required).
        // Must be called BEFORE InputText, since it targets the *next* item.
        if (focus_query_on_open_ || ImGui::IsWindowAppearing())
        {
            ImGui::SetWindowFocus();
            ImGui::ActivateItemByID(query_id);
            ImGui::SetKeyboardFocusHere();
        }
        const bool edited = ImGui::InputText("##command_palette_query", &query_);
        if (ImGui::GetActiveID() == query_id)
            focus_query_on_open_ = false;
        if (edited)
            rebuild_results(ctx);

        // Restore normal spacing after the "BG strip → query" boundary is done.
        ImGui::PopStyleVar();

        const bool pressed_enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        const bool pressed_shift_enter = pressed_enter && ImGui::GetIO().KeyShift;
        const bool pressed_alt_enter = pressed_enter && ImGui::GetIO().KeyAlt;
        // IMPORTANT: When the command palette consumes Enter/Escape, we must prevent the same key press
        // from also being seen by the canvas' hidden InputText / tool layer in the same frame (e.g. Enter
        // confirming an item shouldn't type a character/newline into the canvas).
        //
        // Dear ImGui's legacy IsKeyPressed() polling ignores key ownership unless the key is locked with
        // ImGuiInputFlags_LockThisFrame / LockUntilRelease. Use key ownership locking so non-owner-aware
        // polling code (e.g. canvas CaptureKeyEvents) won't see the key this frame.
        const ImGuiID key_owner = ImGui::GetCurrentWindow()->ID;
        if (pressed_enter)
        {
            ImGui::SetKeyOwner(ImGuiKey_Enter, key_owner, ImGuiInputFlags_LockThisFrame);
            ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, key_owner, ImGuiInputFlags_LockThisFrame);
        }

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
            bool refocus_canvas_on_close = true;

            if (it.kind == Item::Kind::Action)
            {
                // Special-case: tool activation actions (`tool.activate.<tool_id>`) should behave like
                // selecting a Tool item in the palette (activate tool + MRU bump).
                std::string_view tool_id;
                if (IsToolActivateAction(it.id, tool_id))
                {
                    if (ctx.activate_tool_by_id && ToolExists(ctx.tool_palette, tool_id))
                    {
                        ctx.activate_tool_by_id(tool_id);
                        executed = true;
                        MruBump(ctx.session.command_palette.mru_tool_ids, std::string(tool_id), 16);
                    }
                }
                else
                {
                    const app::ActionExecContext host = MakePaletteHostExecContext(ctx);
                    app::RoutedActionExecContext rexec = {
                        .host = host,
                        .tool_palette = ctx.tool_palette,
                        .tool_engine = ctx.tool_engine,
                        .compiled_tool_id = ctx.compiled_tool_id,
                    };
                    executed = app::ExecuteRoutedActionId(it.id, rexec);
                    if (executed)
                        MruBump(ctx.session.command_palette.mru_action_ids, it.id, 32);
                }
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
                // Window/panel operations are about shifting focus to UI chrome, not returning to the canvas.
                // If we execute a window focus/toggle, do not auto-refocus the last canvas on palette close.
                refocus_canvas_on_close = false;
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
                        {
                            if (ctx.request_focus_imgui_window)
                                ctx.request_focus_imgui_window(w.imgui_focus);
                            else
                                ImGui::SetWindowFocus(w.imgui_focus.c_str());
                        }
                        executed = true;
                        MruBump(ctx.session.command_palette.mru_window_keys, w.key, 16);
                        break;
                    }
                    if (it.id == toggle_id)
                    {
                        *w.toggle = !*w.toggle;
                        if (*w.toggle && !w.imgui_focus.empty())
                        {
                            if (ctx.request_focus_imgui_window)
                                ctx.request_focus_imgui_window(w.imgui_focus);
                            else
                                ImGui::SetWindowFocus(w.imgui_focus.c_str());
                        }
                        executed = true;
                        MruBump(ctx.session.command_palette.mru_window_keys, w.key, 16);
                        break;
                    }
                }
            }
            else if (it.kind == Item::Kind::ColourFg || it.kind == Item::Kind::ColourBg)
            {
                // Single source of truth: snap any incoming colour to the active canvas palette
                // so tools (palette indices) and UI (RGB) remain consistent.
                const ImVec4 raw = UnpackRgba32(it.rgba32);
                const ImVec4 col = SnapRgbToActiveCanvasPalette(raw, ctx.active_canvas);
                if (it.kind == Item::Kind::ColourBg)
                {
                    const std::uint32_t before = PackRgba32(ctx.bg_colour);
                    ctx.bg_colour = col;
                    const std::uint32_t after = PackRgba32(ctx.bg_colour);
                    if (after != before)
                    {
                        MruBump(ctx.session.command_palette.mru_bg_rgba32, PackRgba32(col), 16);
                        ctx.session.command_palette.last_executed_item_id = "colour.bg." + std::to_string(PackRgba32(col));
                    }
                }
                else
                {
                    const std::uint32_t before = PackRgba32(ctx.fg_colour);
                    ctx.fg_colour = col;
                    const std::uint32_t after = PackRgba32(ctx.fg_colour);
                    if (after != before)
                    {
                        MruBump(ctx.session.command_palette.mru_fg_rgba32, PackRgba32(col), 16);
                        ctx.session.command_palette.last_executed_item_id = "colour.fg." + std::to_string(PackRgba32(col));
                    }
                }
                executed = true;
            }

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
                    if (refocus_canvas_on_close && ctx.request_focus_last_canvas)
                        ctx.request_focus_last_canvas();
                }
            }
        };

        if (pressed_enter)
        {
            const bool wants_colour_apply = in_colour_mode || colour_lane_interacted_;
            if (wants_colour_apply)
            {
                // Alt+Enter: "Set BG" shortcut.
                // If the user is typing a '#...' colour, apply that typed colour to BG regardless of active_fb.
                // Otherwise, apply the currently selected BG lane item.
                if (pressed_alt_enter)
                {
                    bool did_bg = false;
                    if (in_colour_mode)
                    {
                        std::string q = Trim(query_);
                        std::string_view colour_expr = std::string_view(q);
                        if (q.rfind("fg:", 0) == 0) colour_expr = std::string_view(q).substr(3);
                        else if (q.rfind("bg:", 0) == 0) colour_expr = std::string_view(q).substr(3);

                        if (!colour_expr.empty() && colour_expr.front() == '#')
                        {
                            if (auto col = ParseHexColour(colour_expr))
                            {
                                Item it;
                                it.kind = Item::Kind::ColourBg;
                                it.id = std::string(colour_expr);
                                it.label = "Typed";
                                it.detail = std::string(colour_expr);
                                it.rgba32 = PackRgba32(*col);
                                execute_item(it, /*keep_palette_open=*/true);
                                did_bg = true;
                            }
                        }
                    }
                    if (!did_bg)
                    {
                        if (!bg_lane_.empty() && bg_lane_index_ >= 0 && bg_lane_index_ < (int)bg_lane_.size())
                            execute_item(bg_lane_[(size_t)bg_lane_index_], /*keep_palette_open=*/true);
                    }
                }
                else
                {
                    // Default: Apply BOTH FG and BG from their respective lane selections.
                    if (!fg_lane_.empty() && fg_lane_index_ >= 0 && fg_lane_index_ < (int)fg_lane_.size())
                        execute_item(fg_lane_[(size_t)fg_lane_index_], /*keep_palette_open=*/true);
                    if (!bg_lane_.empty() && bg_lane_index_ >= 0 && bg_lane_index_ < (int)bg_lane_.size())
                        execute_item(bg_lane_[(size_t)bg_lane_index_], /*keep_palette_open=*/true);
                }

                if (!pressed_shift_enter)
                {
                    Close();
                    if (ctx.request_focus_last_canvas) ctx.request_focus_last_canvas();
                }
            }
            else
            {
                if (!results_.empty() && selected_index_ >= 0 && selected_index_ < (int)results_.size())
                    execute_item(results_[(size_t)selected_index_], /*keep_palette_open=*/pressed_shift_enter);
            }
        }

        ImGui::EndPopup();
        return;
    }

    // Popup got closed externally.
    is_open_ = false;
    focus_query_on_open_ = false;
}


