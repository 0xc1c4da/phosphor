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

// Minimal QuickJS-based scripting engine for ANSL-style layer manipulation.
//
// Conventions for user scripts (evaluated as global script, not a module):
// - Define a global function: `function render(ctx, layer) { ... }`
//   where `ctx` is {cols, rows, frame, time} and `layer` provides:
//     - layer.set(x, y, cpOrString)
//     - layer.get(x, y) -> string (single glyph)
//     - layer.clear(cpOrString?)
class AnslScriptEngine
{
public:
    AnslScriptEngine();
    ~AnslScriptEngine();

    AnslScriptEngine(const AnslScriptEngine&) = delete;
    AnslScriptEngine& operator=(const AnslScriptEngine&) = delete;

// Initializes builtins like console.log and registers the host `ANSL` library.
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


