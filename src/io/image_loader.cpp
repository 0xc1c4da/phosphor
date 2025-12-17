#include "io/image_loader.h"

#include <cstring>

// stb_image implementation must live in exactly one translation unit.
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace image_loader
{
bool LoadImageAsRgba32(const std::string& path,
                       int& out_width,
                       int& out_height,
                       std::vector<unsigned char>& out_pixels,
                       std::string& err)
{
    err.clear();
    out_width = 0;
    out_height = 0;
    out_pixels.clear();

    int w = 0;
    int h = 0;
    int channels_in_file = 0;

    // Force 4 channels so we always get RGBA8.
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels_in_file, 4);
    if (!data)
    {
        err = std::string("Failed to load image: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
        return false;
    }

    if (w <= 0 || h <= 0)
    {
        stbi_image_free(data);
        err = "Invalid image dimensions.";
        return false;
    }

    out_width = w;
    out_height = h;

    const size_t pixel_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    out_pixels.resize(pixel_bytes);
    std::memcpy(out_pixels.data(), data, pixel_bytes);

    stbi_image_free(data);
    return true;
}
} // namespace image_loader


