#pragma once

#include "ansl/ansl_script_engine.h"

// Shared IMGUI widget for rendering host-managed ANSL parameters (settings.params -> ctx.params).
//
// Usage:
//   if (ImGui::CollapsingHeader("Parameters")) { RenderAnslParamsUI("params", engine); }
//
// Returns true if any parameter value was changed this frame.
struct AnslParamsUISkipList
{
    const char* const* keys = nullptr;
    int                count = 0;
};

// Render only the compact primary bar (spec.primary == true), with optional skip list.
bool RenderAnslParamsUIPrimaryBar(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip);

// Render advanced params (spec.primary == false) grouped by section, with optional skip list.
// No outer "More…" wrapper is added by this function (caller controls placement).
bool RenderAnslParamsUIAdvanced(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip);

// Render all params (primary + advanced) with optional skip list (by spec.key).
bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine, const AnslParamsUISkipList* skip);

// Convenience overload (no skip list).
inline bool RenderAnslParamsUI(const char* id, AnslScriptEngine& engine)
{
    return RenderAnslParamsUI(id, engine, nullptr);
}

// Render a single param control by key. Returns true if changed.
// Returns false if the param key doesn't exist or couldn't be rendered.
bool RenderAnslParamByKey(const char* id, AnslScriptEngine& engine, const char* key, bool compact);


