#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "core/palette/palette.h"

class AnsiCanvas;

namespace textmode_font
{
class Registry;
struct SanityCache;
}

enum class AnslParamType
{
    Bool,
    Int,
    Float,
    Enum,
    Button, // edge-triggered; true only for the frame it is clicked
};

struct AnslParamSpec
{
    std::string key;   // lua identifier under ctx.params.<key>
    std::string label; // human-friendly label for UI (optional)
    std::string tooltip; // optional help text (shown in UI tooltip)
    AnslParamType type = AnslParamType::Bool;

    // Optional layout hints for the host parameter UI.
    // If true, render this parameter on the same line as the previous one.
    bool inline_with_prev = false;

    // Optional grouping for host UI (e.g. "Brush", "Sampling", "Advanced").
    // Empty means "General".
    std::string section;

    // Optional enablement condition: when set, this param is disabled unless
    // ctx.params.<enabled_if> is true (bool param).
    std::string enabled_if;

    // Optional UI hint:
    // - "auto" (default)
    // - "toggle"      (bool)
    // - "checkbox"    (bool)
    // - "slider"      (int/float)
    // - "drag"        (int/float)
    // - "segmented"   (enum)
    // - "combo"       (enum)
    // - "action"      (button)
    std::string ui;

    // If true, this param should be shown in the compact "options bar" region.
    bool primary = false;

    // Optional width hint for the widget (ImGui item width in px). 0 means auto.
    float width = 0.0f;

    // Optional explicit ordering. If multiple params share the same order, we fall back to label/key.
    int  order = 0;
    bool order_set = false;

    // Int/Float ranges (optional; if min==max, UI may choose a simple input widget).
    int   int_min = 0;
    int   int_max = 0;
    int   int_step = 1;
    float float_min = 0.0f;
    float float_max = 0.0f;
    float float_step = 0.1f;

    // Enum items: list of string options.
    std::vector<std::string> enum_items;
};

// Current (host-managed) value for a script parameter.
struct AnslParamValue
{
    AnslParamType type = AnslParamType::Bool;
    bool          b = false;
    int           i = 0;
    float         f = 0.0f;
    std::string   s; // enum current value (string)
};

struct AnslFrameContext
{
    int    cols  = 0;
    int    rows  = 0;
    int    frame = 0;
    double time  = 0.0; // milliseconds (ANSL compatibility)

    // Compatibility with classic ANSL runner context.
    float metrics_aspect = 1.0f;

    // Current "format/attributes" selection for tools (bitmask of AnsiCanvas::Attr_*).
    // 0 means "no attributes".
    std::uint32_t attrs = 0;

    // Caret = the editing caret used by keyboard operations (cell space).
    int caret_x = 0;
    int caret_y = 0;

    // Whether the canvas has keyboard focus this frame.
    bool focused = false;

    // Tool/script phase hint:
    //  - 0 = keyboard phase (host runs tool before computing canvas size)
    //  - 1 = mouse phase (host runs tool after updating cursor state for this frame)
    int phase = 0;

    // Cursor = the mouse cursor expressed in cell space, plus button state.
    int  cursor_x = 0;
    int  cursor_y = 0;
    int  cursor_half_y = 0;   // y in half-rows (cursor_y*2 + 0/1)
    bool cursor_left_down = false;
    bool cursor_right_down = false;
    int  cursor_px = 0;
    int  cursor_py = 0;
    int  cursor_phalf_y = 0;
    bool cursor_prev_left_down = false;
    bool cursor_prev_right_down = false;
    bool cursor_valid = false;

    // Typed codepoints collected from ImGui text input (UTF-32 codepoints).
    // The host owns the vector; valid only for the duration of RunFrame().
    const std::vector<char32_t>* typed = nullptr;

    // Discrete key press events (match ImGui::IsKeyPressed semantics).
    bool key_left = false;
    bool key_right = false;
    bool key_up = false;
    bool key_down = false;
    bool key_home = false;
    bool key_end = false;
    bool key_backspace = false;
    bool key_delete = false;
    bool key_enter = false;
    // Additional key press events for tool shortcuts (selection, clipboard, cancel).
    bool key_c = false;
    bool key_v = false;
    bool key_x = false;
    bool key_a = false;
    bool key_escape = false;

    // Modifier state (captured this frame).
    bool mod_ctrl = false;
    bool mod_shift = false;
    bool mod_alt = false;
    bool mod_super = false;

    // Engine-provided hotkeys/actions (preferred over "typed-char hacks").
    // These are computed by the host keybinding engine so tools don't need to
    // re-encode platform differences or modifier edge cases.
    struct Hotkeys
    {
        bool copy = false;
        bool cut = false;
        bool paste = false;
        bool selectAll = false;
        bool cancel = false;
        bool deleteSelection = false;
    } hotkeys;

    // List of action ids that are pressed this frame (edge-triggered).
    // The host owns the vector; valid only for the duration of RunFrame().
    const std::vector<std::string>* actions_pressed = nullptr;

    // Script defaults (palette indices in the active canvas palette). -1 means "unset".
    // These are not yet used by the host shim, but are exposed so scripts can
    // pick up the editor's current FG/BG selection without guessing.
    int fg = -1;
    int bg = -1;

    // Active palette identity for this canvas.
    // Tools should interpret ctx.fg/ctx.bg and ctx.palette as indices in this palette.
    bool          palette_is_builtin = true;
    std::uint32_t palette_builtin = (std::uint32_t)phos::color::BuiltinPalette::Xterm256;

    // Current single-cell glyph selection for tools (UTF-8). Empty means " " (space).
    // Provided by the host (e.g. character picker/palette selection).
    std::string_view glyph_utf8;
    // Optional glyph codepoint (Unicode scalar). 0 means "unknown".
    int glyph_cp = 0;
    // Optional glyph token (GlyphId; lossless). 0 means "unknown/unset".
    std::uint32_t glyph_id = 0;

    // Optional multi-cell brush stamp (a rectangular block of cells).
    // This is separate from the single-cell glyph selection above:
    // - `ctx.glyph` is one UTF-8 glyph for pencil-like tools
    // - `ctx.brush` is a multi-cell stamp defined by the host (e.g. captured from selection)
    struct BrushStamp
    {
        int w = 0;
        int h = 0;
        // Arrays are row-major, length = w*h.
        // - glyph: GlyphId tokens (lossless; may exceed Unicode range)
        // - cp: best-effort Unicode representative (legacy; may be null)
        // - fg/bg: palette indices in ctx.palette (kUnsetIndex16 = unset)
        // - attrs: attribute bitmask, 0 = none
        const std::uint32_t*  glyph = nullptr;
        const char32_t*      cp = nullptr;
        const std::uint16_t* fg = nullptr;
        const std::uint16_t* bg = nullptr;
        const std::uint16_t* attrs = nullptr;
    };
    // If null, ctx.brush is considered empty/unset.
    const BrushStamp* brush = nullptr;

    // Optional restriction: list of allowed indices (in the active palette index space).
    // If provided, tools should quantize/snap any computed colors to this list.
    // Host owns the vector; valid only for the duration of RunFrame().
    const std::vector<int>* allowed_indices = nullptr;

    // Optional glyph candidate list provided by the host (Unicode scalar values).
    // Intended to constrain expensive glyph searches (e.g. deform quantization) to a small set
    // such as the Character Palette + glyphs already used on the canvas.
    //
    // Host owns the vector; valid only for the duration of RunFrame().
    const std::vector<std::uint32_t>* glyph_candidates = nullptr;

    // Optional glyph candidate list provided by the host (GlyphId tokens; lossless).
    // Values may be:
    // - Unicode scalar values (0..0x10FFFF), or
    // - token-space GlyphIds (>= 0x80000000).
    //
    // This is the preferred candidate list for token-aware tools. Existing tools can continue
    // using `glyph_candidates` (codepoints) for backward compatibility.
    //
    // Host owns the vector; valid only for the duration of RunFrame().
    const std::vector<std::uint32_t>* glyph_id_candidates = nullptr;

    // If true, host will read ctx.caret.{x,y} back after the script and apply it to the canvas.
    bool allow_caret_writeback = false;
};

// -------------------------------------------------------------------------
// Tool commands (Lua tool scripts -> host)
// -------------------------------------------------------------------------
//
// Tools emit commands by appending to `ctx.out` (an array) during render():
//
//   ctx.out[#ctx.out + 1] = { type = "palette.set", fg = 123, bg = 45 }
//   ctx.out[#ctx.out + 1] = { type = "brush.set", cp = 0x2588 } -- Unicode scalar value
//   ctx.out[#ctx.out + 1] = { type = "tool.activate_prev" }
//   ctx.out[#ctx.out + 1] = { type = "tool.activate", id = "pencil" }
//
// The host clears `ctx.out` each frame before calling render(). Commands are
// processed in order after render() returns. This is intentionally generic:
// any tool can request host-side actions without needing tool-specific fields.
struct ToolCommand
{
    enum class Type
    {
        PaletteSet,
        BrushSet,
        AttrsSet,
        ToolActivatePrev,
        ToolActivate,
        CanvasCropToSelection,
        BrushPreviewSet,
    };

    Type type = Type::PaletteSet;

    // PaletteSet
    bool has_fg = false;
    int  fg = -1; // palette index (in the canvas's active palette index space)
    bool has_bg = false;
    int  bg = -1; // palette index (in the canvas's active palette index space)

    // BrushSet
    uint32_t brush_cp = 0; // Unicode scalar value (>=0). Note: this sets ctx.glyph (single-cell), not ctx.brush.

    // AttrsSet
    std::uint32_t attrs = 0; // bitmask of AnsiCanvas::Attr_* (host-defined)

    // ToolActivate
    std::string tool_id;

    // BrushPreviewSet
    // The tool can request a transient brush preview overlay (e.g. size indicator).
    // Host is expected to clear the preview each frame unless it is re-sent.
    //
    // Two modes:
    // 1) Anchor-based (centered on cursor or caret):
    //    - anchor = "cursor" (default) or "caret"
    //    - rx/ry are half-extents in cells (0 => single-cell). Rect is inclusive.
    //    - ox/oy are integer offsets applied after anchoring.
    // 2) Explicit rect:
    //    - set has_rect=true and specify x0,y0,x1,y1 (inclusive, may be outside canvas).
    enum class BrushPreviewAnchor
    {
        Cursor = 0,
        Caret = 1,
    };
    BrushPreviewAnchor preview_anchor = BrushPreviewAnchor::Cursor;
    int  preview_rx = 0;
    int  preview_ry = 0;
    int  preview_ox = 0;
    int  preview_oy = 0;
    bool preview_has_rect = false;
    int  preview_x0 = 0;
    int  preview_y0 = 0;
    int  preview_x1 = 0;
    int  preview_y1 = 0;
};

struct ToolCommandSink
{
    // If true, the engine will read ctx.out after the script and append parsed commands here.
    bool allow_tool_commands = false;
    // Host-owned; valid only for the duration of RunFrame().
    std::vector<ToolCommand>* out_commands = nullptr;
};

// Script-provided settings (optional).
// Lua-side convention (preferred):
//   settings = { fps = 60, once = false, fg = "#ffffff", bg = "#000000" }
// Colors can be:
// - a palette index (0..paletteSize-1 in the active canvas palette), or
// - "#RRGGBB"/"RRGGBB" string (mapped to nearest index in the active canvas palette).
struct AnslScriptSettings
{
    bool has_fps = false;
    int  fps = 30; // valid if has_fps

    // If true, the host should run the script once (static frame) rather than continuously.
    bool once = false;

    // Optional layer foreground/background fill to apply by the host.
    // Stored as indices in the active canvas palette index space.
    bool has_foreground = false;
    int  foreground_xterm = 0;
    bool has_background = false;
    int  background_xterm = 0;
};

// Minimal LuaJIT-based scripting engine for ANSL-style layer manipulation.
//
// Conventions for user scripts:
// - Define a global function: `function render(ctx, layer) ... end`
//   where `ctx` is {cols, rows, frame, time, metrics={aspect=...}, cursor={...}}
//   and `layer` is a userdata supporting:
//     - layer:set(x, y, cpOrString)
//     - layer:get(x, y) -> ch, fg, bg
//         - ch: string (single glyph)
//         - fg/bg: indices in the active canvas palette (or nil when "unset")
//     - layer:clear(cpOrString?)
//     - layer:setRow(y, utf8String)
//
// Classic ANSL compatibility:
// - If `render` is missing but global `main(coord, context, cursor, buffer)` exists,
//   the host will generate a default render() that:
//   - calls optional `pre(context, cursor, buffer)` once per frame
//   - runs `main()` per cell
//   - supports `main()` returning either:
//       - a scalar (string/number) -> glyph
//       - a table with `char` (+ optional `fg`/`bg` palette indices) -> per-cell colors via layer:set()
//   - calls optional `post(context, cursor, buffer)` once per frame
class AnslScriptEngine
{
public:
    AnslScriptEngine();
    ~AnslScriptEngine();

    AnslScriptEngine(const AnslScriptEngine&) = delete;
    AnslScriptEngine& operator=(const AnslScriptEngine&) = delete;

    // Initializes the Lua state and registers the host `ansl` module (also published as global `ansl`).
    // If `font_cache` is provided, Init will scan fonts using it and can optionally run an
    // expensive "validate all fonts" pass on cache miss (to populate broken ids).
    bool Init(const std::string& assets_dir,
              std::string& error,
              textmode_font::SanityCache* font_cache = nullptr,
              bool validate_fonts_if_cache_miss = false);

    // Compiles/evaluates user script and caches the `render` function.
    //
    // IMPORTANT: Some scripts compute color constants at load time (e.g. `local c = ansl.color.hex("#ff00ff")`).
    // Since `ansl.color.*` depends on the active canvas palette, the host should pass the target canvas here
    // so compilation happens with the correct active palette binding.
    bool CompileUserScript(const std::string& source, std::string& error);
    bool CompileUserScript(const std::string& source, const AnsiCanvas* canvas, std::string& error);

    // Runs cached render() for a frame against the given canvas/layer.
    bool RunFrame(AnsiCanvas& canvas,
                  int layer_index,
                  const AnslFrameContext& frame_ctx,
                  const ToolCommandSink& tool_cmds,
                  bool clear_layer_first,
                  std::string& error);

    bool HasRenderFunction() const;
    AnslScriptSettings GetSettings() const;

    // ---------------------------------------------------------------------
    // Parameters (settings.params -> ctx.params)
    // ---------------------------------------------------------------------
    bool HasParams() const;
    const std::vector<AnslParamSpec>& GetParamSpecs() const;
    bool GetParamBool(const std::string& key, bool& out) const;
    bool GetParamInt(const std::string& key, int& out) const;
    bool GetParamFloat(const std::string& key, float& out) const;
    bool GetParamEnum(const std::string& key, std::string& out) const;

    bool SetParamBool(const std::string& key, bool v);
    bool SetParamInt(const std::string& key, int v);
    bool SetParamFloat(const std::string& key, float v);
    bool SetParamEnum(const std::string& key, std::string v);
    bool FireParamButton(const std::string& key); // sets a button param true for the next frame

    void ResetParamsToDefaults();

    // ---------------------------------------------------------------------
    // Fonts (textmode_font::Registry backing ansl.font.*)
    // ---------------------------------------------------------------------
    const textmode_font::Registry* GetFontRegistry() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};


