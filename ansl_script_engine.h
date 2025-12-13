#pragma once

#include <string>

class AnsiCanvas;

struct AnslFrameContext
{
    int    cols  = 0;
    int    rows  = 0;
    int    frame = 0;
    double time  = 0.0; // milliseconds (ANSL compatibility)

    // Compatibility with classic ANSL runner context.
    float metrics_aspect = 1.0f;

    // Cursor/pointer in cell space.
    int  cursor_x = 0;
    int  cursor_y = 0;
    bool cursor_pressed = false;
    int  cursor_px = 0;
    int  cursor_py = 0;
    bool cursor_ppressed = false;
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
//   the host will generate a default render() that runs main per cell and calls setRow().
class AnslScriptEngine
{
public:
    AnslScriptEngine();
    ~AnslScriptEngine();

    AnslScriptEngine(const AnslScriptEngine&) = delete;
    AnslScriptEngine& operator=(const AnslScriptEngine&) = delete;

    // Initializes the Lua state and registers the host `ansl` module (also published as global `ANSL`).
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

private:
    struct Impl;
    Impl* impl_ = nullptr;
};


