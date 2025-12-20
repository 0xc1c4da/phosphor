#pragma once

#include <string>
#include <vector>

struct AnsiCanvas;

namespace open_canvas_cache
{
// Returns the absolute directory used for caching session canvases:
//   <config_dir>/cache/session_canvases
std::string GetSessionCanvasCacheDir();

// Saves a canvas project as a .phos file in the session cache directory.
// - out_rel_path is a relative path under the cache dir, suitable for storing in session.json
//   (e.g. "session_canvases/canvas_12.phos").
bool SaveCanvasToSessionCachePhos(int canvas_id,
                                 const AnsiCanvas& canvas,
                                 std::string& out_rel_path,
                                 std::string& err);

// Loads a cached .phos project into out_canvas.
// Accepts either an absolute path or a cache-relative path like "session_canvases/canvas_12.phos".
bool LoadCanvasFromSessionCachePhos(const std::string& rel_or_abs_path,
                                   AnsiCanvas& out_canvas,
                                   std::string& err);

// Best-effort delete of a cache file (accepts either cache-relative or absolute path).
// Returns true if the file was removed, or if it didn't exist.
bool DeleteSessionCanvasCachePhos(const std::string& rel_or_abs_path, std::string& err);

// Best-effort cleanup: removes cached .phos files in session_canvases/ that are not in the
// provided set of cache-relative paths.
void PruneSessionCanvasCache(const std::vector<std::string>& keep_rel_paths);
} // namespace open_canvas_cache


