#pragma once

#include <string>

struct AnsiCanvas;

namespace project_file
{
// Save/load Phosphor project files (*.phos).
//
// Format: zstd-wrapped CBOR with a small header (see implementation).
// These helpers are used by both the File menu (IoManager) and session cache.
bool SaveProjectToFile(const std::string& path, const AnsiCanvas& canvas, std::string& err);
bool LoadProjectFromFile(const std::string& path, AnsiCanvas& out_canvas, std::string& err);
} // namespace project_file


