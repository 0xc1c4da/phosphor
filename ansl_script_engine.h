#pragma once

#include <string>
#include <vector>

class AnsiCanvas;

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
    bool cursor_left_down = false;
    bool cursor_right_down = false;
    int  cursor_px = 0;
    int  cursor_py = 0;
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

    // Script defaults (xterm-256 indices). -1 means "unset".
    // These are not yet used by the host shim, but are exposed so scripts can
    // pick up the editor's current FG/BG selection without guessing.
    int fg = -1;
    int bg = -1;

    // If true, host will read ctx.caret.{x,y} back after the script and apply it to the canvas.
    bool allow_caret_writeback = false;
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
//     - layer:get(x, y) -> string (single glyph)
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
                  bool clear_layer_first,
                  std::string& error);

    bool HasRenderFunction() const;
    AnslScriptSettings GetSettings() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};


