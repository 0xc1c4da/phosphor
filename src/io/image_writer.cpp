#include "io/image_writer.h"

#include <algorithm>
#include <cstring>

// stb_image_write implementation must live in exactly one translation unit.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace image_writer
{
bool WriteJpgFromRgba32(const std::string& path,
                        int width,
                        int height,
                        const std::vector<std::uint8_t>& rgba,
                        int quality,
                        std::string& err)
{
    err.clear();
    if (width <= 0 || height <= 0)
    {
        err = "Invalid image dimensions.";
        return false;
    }
    const size_t need = (size_t)width * (size_t)height * 4u;
    if (rgba.size() < need)
    {
        err = "Invalid RGBA buffer size.";
        return false;
    }

    quality = std::clamp(quality, 1, 100);

    // stb writes JPEG from RGB; drop alpha.
    std::vector<std::uint8_t> rgb;
    rgb.resize((size_t)width * (size_t)height * 3u);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t si = ((size_t)y * (size_t)width + (size_t)x) * 4u;
            const size_t di = ((size_t)y * (size_t)width + (size_t)x) * 3u;
            rgb[di + 0] = rgba[si + 0];
            rgb[di + 1] = rgba[si + 1];
            rgb[di + 2] = rgba[si + 2];
        }
    }

    const int ok = stbi_write_jpg(path.c_str(), width, height, 3, rgb.data(), quality);
    if (!ok)
    {
        err = "stbi_write_jpg() failed.";
        return false;
    }
    return true;
}
} // namespace image_writer


