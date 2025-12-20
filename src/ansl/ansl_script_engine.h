#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cstdint>

class AnsiCanvas;

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
    AnslParamType type = AnslParamType::Bool;

    // Optional layout hints for the host parameter UI.
    // If true, render this parameter on the same line as the previous one.
    bool same_line = false;

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

    // Script defaults (xterm-256 indices). -1 means "unset".
    // These are not yet used by the host shim, but are exposed so scripts can
    // pick up the editor's current FG/BG selection without guessing.
    int fg = -1;
    int bg = -1;

    // Current "brush" glyph selection for tools (UTF-8). Empty means " " (space).
    // Provided by the host (e.g. character picker/palette selection).
    std::string_view brush_utf8;
    // Optional brush codepoint (Unicode scalar). 0 means "unknown".
    int brush_cp = 0;

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
        ToolActivatePrev,
        ToolActivate,
        CanvasCropToSelection,
    };

    Type type = Type::PaletteSet;

    // PaletteSet
    bool has_fg = false;
    int  fg = -1; // xterm-256 index (0..255)
    bool has_bg = false;
    int  bg = -1; // xterm-256 index (0..255)

    // BrushSet
    uint32_t brush_cp = 0; // Unicode scalar value (>=0)

    // ToolActivate
    std::string tool_id;
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
// - xterm-256 index (0..255), or
// - "#RRGGBB"/"RRGGBB" string (mapped to nearest xterm-256).
struct AnslScriptSettings
{
    bool has_fps = false;
    int  fps = 30; // valid if has_fps

    // If true, the host should run the script once (static frame) rather than continuously.
    bool once = false;

    // Optional layer foreground/background fill to apply by the host.
    // Stored as xterm-256 indices (0..255).
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
//         - fg/bg: xterm-256 indices (0..255) or nil when "unset"
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
//       - a table with `char` (+ optional `fg`/`bg` xterm-256 indices) -> per-cell colors via layer:set()
//   - calls optional `post(context, cursor, buffer)` once per frame
class AnslScriptEngine
{
public:
    AnslScriptEngine();
    ~AnslScriptEngine();

    AnslScriptEngine(const AnslScriptEngine&) = delete;
    AnslScriptEngine& operator=(const AnslScriptEngine&) = delete;

    // Initializes the Lua state and registers the host `ansl` module (also published as global `ansl`).
    bool Init(const std::string& assets_dir, std::string& error);

    // Compiles/evaluates user script and caches the `render` function.
    bool CompileUserScript(const std::string& source, std::string& error);

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

private:
    struct Impl;
    Impl* impl_ = nullptr;
};


