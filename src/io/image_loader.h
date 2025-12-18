#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_loader
{
// Load an image from disk into an RGBA8 buffer using stb_image.
// - supports common formats (PNG/JPG/GIF/BMP/...)
// - output pixels are row-major, width * height * 4 bytes.
bool LoadImageAsRgba32(const std::string& path,
                       int& out_width,
                       int& out_height,
                       std::vector<unsigned char>& out_pixels,
                       std::string& err);

// Decode an image from memory into an RGBA8 buffer using stb_image.
// `bytes` can contain PNG/JPG/GIF/BMP/etc.
bool LoadImageFromMemoryAsRgba32(const std::vector<std::uint8_t>& bytes,
                                 int& out_width,
                                 int& out_height,
                                 std::vector<unsigned char>& out_pixels,
                                 std::string& err);
} // namespace image_loader


