#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_writer
{
// Writes an RGBA8 buffer to a JPEG file (RGB output; alpha is ignored).
// Returns false on error and sets `err`.
bool WriteJpgFromRgba32(const std::string& path,
                        int width,
                        int height,
                        const std::vector<std::uint8_t>& rgba,
                        int quality,
                        std::string& err);
} // namespace image_writer


