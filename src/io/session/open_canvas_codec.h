#pragma once

#include "core/canvas.h"
#include "io/session/session_state.h"

#include <cstdint>
#include <string>

// Session-owned encoding for embedding an AnsiCanvas::ProjectState into SessionState::OpenCanvas.
//
// Current format:
// - ProjectState -> nlohmann::json (see project_state_json)
// - json -> CBOR (nlohmann::json::to_cbor)
// - CBOR bytes -> zstd compress
// - zstd bytes -> base64 string (stored in session.json)
//
// Rationale: keep session.json reasonably small while remaining a single-file persistence format.
namespace open_canvas_codec
{
// Decodes oc.project_cbor_{size,zstd_b64} into out_ps.
// Returns false if the OpenCanvas has no embedded project, or if decoding fails.
bool DecodeProjectState(const SessionState::OpenCanvas& oc, AnsiCanvas::ProjectState& out_ps, std::string& err);

// Encodes ps into oc.project_cbor_{size,zstd_b64}.
bool EncodeProjectState(const AnsiCanvas::ProjectState& ps, SessionState::OpenCanvas& oc, std::string& err);
} // namespace open_canvas_codec


