#include "io/formats/image.h"

#include "core/canvas_rasterizer.h"
#include "core/color_system.h"
#include "core/xterm256_palette.h"
#include "io/image_loader.h"
#include "io/image_writer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

// LodePNG (provided via Nix flake input; compiled via src/io/lodepng_unit.cpp).
#include <lodepng.h>

namespace formats
{
namespace image
{
namespace
{
static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string FileExtLower(const std::string& path)
{
    namespace fs = std::filesystem;
    std::string ext;
    try
    {
        fs::path p(path);
        ext = p.extension().string();
    }
    catch (...)
    {
    }
    if (!ext.empty() && ext[0] == '.')
        ext.erase(ext.begin());
    return ToLowerAscii(ext);
}

static inline void UnpackImGui(std::uint32_t c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a)
{
    r = (std::uint8_t)(c & 0xFF);
    g = (std::uint8_t)((c >> 8) & 0xFF);
    b = (std::uint8_t)((c >> 16) & 0xFF);
    a = (std::uint8_t)((c >> 24) & 0xFF);
}

static void ConfigurePngCompression(LodePNGState& state, int compression)
{
    // lodepng doesn't expose a single "compression level" knob like zlib, so we approximate.
    const int lvl = std::clamp(compression, 0, 9);
    if (lvl <= 0)
    {
        state.encoder.zlibsettings.btype = 0;    // uncompressed blocks
        state.encoder.zlibsettings.use_lz77 = 0; // no LZ77
        return;
    }
    state.encoder.zlibsettings.btype = 2; // dynamic Huffman
    state.encoder.zlibsettings.use_lz77 = 1;
    state.encoder.zlibsettings.windowsize = (lvl >= 6) ? 32768u : 2048u;
    state.encoder.zlibsettings.minmatch = 3;
    state.encoder.zlibsettings.nicematch = (lvl >= 7) ? 258u : 128u;
    state.encoder.zlibsettings.lazymatching = 1;
}

static void QuantizeToXterm16(const std::vector<std::uint8_t>& rgba,
                              int w, int h,
                              std::vector<std::uint8_t>& out_idx,
                              std::vector<std::uint8_t>& out_palette_rgba)
{
    out_idx.assign((size_t)w * (size_t)h, 0u);
    out_palette_rgba.clear();
    out_palette_rgba.resize(16u * 4u, 255u);

    // Palette entries = xterm indices 0..15.
    for (int i = 0; i < 16; ++i)
    {
        const std::uint32_t c = xterm256::Color32ForIndex(i);
        std::uint8_t r, g, b, a;
        UnpackImGui(c, r, g, b, a);
        out_palette_rgba[(size_t)i * 4u + 0] = r;
        out_palette_rgba[(size_t)i * 4u + 1] = g;
        out_palette_rgba[(size_t)i * 4u + 2] = b;
        out_palette_rgba[(size_t)i * 4u + 3] = 255;
    }

    // Quantize each pixel to nearest among the 16 entries.
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t si = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            const std::uint8_t r = rgba[si + 0];
            const std::uint8_t g = rgba[si + 1];
            const std::uint8_t b = rgba[si + 2];
            // Choose nearest xterm index, then clamp to 0..15 by searching only those.
            int best = 0;
            int best_d2 = 0x7fffffff;
            for (int i = 0; i < 16; ++i)
            {
                const xterm256::Rgb& p = xterm256::RgbForIndex(i);
                const int dr = (int)r - (int)p.r;
                const int dg = (int)g - (int)p.g;
                const int db = (int)b - (int)p.b;
                const int d2 = dr * dr + dg * dg + db * db;
                if (d2 < best_d2)
                {
                    best_d2 = d2;
                    best = i;
                }
            }
            out_idx[(size_t)y * (size_t)w + (size_t)x] = (std::uint8_t)best;
        }
    }
}

static void QuantizeToXterm256(const std::vector<std::uint8_t>& rgba,
                               int w, int h,
                               std::vector<std::uint8_t>& out_idx,
                               std::vector<std::uint8_t>& out_palette_rgba)
{
    out_idx.assign((size_t)w * (size_t)h, 0u);
    out_palette_rgba.clear();
    out_palette_rgba.resize(256u * 4u, 255u);

    for (int i = 0; i < 256; ++i)
    {
        const std::uint32_t c = xterm256::Color32ForIndex(i);
        std::uint8_t r, g, b, a;
        UnpackImGui(c, r, g, b, a);
        out_palette_rgba[(size_t)i * 4u + 0] = r;
        out_palette_rgba[(size_t)i * 4u + 1] = g;
        out_palette_rgba[(size_t)i * 4u + 2] = b;
        out_palette_rgba[(size_t)i * 4u + 3] = 255;
    }

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t si = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            const std::uint8_t r = rgba[si + 0];
            const std::uint8_t g = rgba[si + 1];
            const std::uint8_t b = rgba[si + 2];
            const int idx = xterm256::NearestIndex(r, g, b);
            out_idx[(size_t)y * (size_t)w + (size_t)x] = (std::uint8_t)idx;
        }
    }
}

static void QuantizeToXterm240Safe(const std::vector<std::uint8_t>& rgba,
                                   int w, int h,
                                   std::vector<std::uint8_t>& out_idx,
                                   std::vector<std::uint8_t>& out_palette_rgba)
{
    // Palette entries correspond to xterm indices 16..255 (240 colors).
    out_idx.assign((size_t)w * (size_t)h, 0u);
    out_palette_rgba.clear();
    out_palette_rgba.resize(240u * 4u, 255u);

    for (int i = 0; i < 240; ++i)
    {
        const int xterm_index = 16 + i;
        const std::uint32_t c = xterm256::Color32ForIndex(xterm_index);
        std::uint8_t r, g, b, a;
        UnpackImGui(c, r, g, b, a);
        out_palette_rgba[(size_t)i * 4u + 0] = r;
        out_palette_rgba[(size_t)i * 4u + 1] = g;
        out_palette_rgba[(size_t)i * 4u + 2] = b;
        out_palette_rgba[(size_t)i * 4u + 3] = 255;
    }

    // Quantize each pixel to nearest among xterm indices 16..255.
    //
    // Refactor note: this used to scan 240 entries per pixel. We now use a coarse RGB->index 3D LUT
    // cached in the core LUT cache (5 bits/channel => 32^3 entries).
    auto& cs = phos::color::GetColorSystem();
    const phos::color::PaletteInstanceId pal240 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Xterm240Safe);
    const phos::color::QuantizePolicy qpol = phos::color::DefaultQuantizePolicy();
    const std::shared_ptr<const phos::color::RgbQuantize3dLut> qlut =
        cs.Luts().GetOrBuildQuant3d(cs.Palettes(), pal240, /*bits=*/5, qpol);

    const std::uint8_t bits = qlut ? qlut->bits : 0;
    const std::size_t side = (bits > 0) ? ((std::size_t)1u << bits) : 0u;
    const int shift = (bits > 0) ? (8 - (int)bits) : 0;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t si = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            const std::uint8_t r = rgba[si + 0];
            const std::uint8_t g = rgba[si + 1];
            const std::uint8_t b = rgba[si + 2];

            std::uint8_t best = 0; // palette index 0..239
            if (qlut && side > 0)
            {
                const std::size_t rx = (std::size_t)(r >> shift);
                const std::size_t gy = (std::size_t)(g >> shift);
                const std::size_t bz = (std::size_t)(b >> shift);
                const std::size_t flat = (bz * side + gy) * side + rx;
                // LUT stores palette index in the *palette* (0..239).
                best = qlut->table[flat];
            }
            else
            {
                // Fallback: brute-force scan.
                int best_d2 = 0x7fffffff;
                for (int i = 0; i < 240; ++i)
                {
                    const int xterm_index = 16 + i;
                    const xterm256::Rgb& p = xterm256::RgbForIndex(xterm_index);
                    const int dr = (int)r - (int)p.r;
                    const int dg = (int)g - (int)p.g;
                    const int db = (int)b - (int)p.b;
                    const int d2 = dr * dr + dg * dg + db * db;
                    if (d2 < best_d2)
                    {
                        best_d2 = d2;
                        best = (std::uint8_t)i;
                    }
                }
            }
            out_idx[(size_t)y * (size_t)w + (size_t)x] = best;
        }
    }
}

static bool WritePngIndexed(const std::string& path,
                            int w,
                            int h,
                            const std::vector<std::uint8_t>& indices,
                            const std::vector<std::uint8_t>& palette_rgba,
                            int bitdepth,
                            int compression,
                            std::string& err)
{
    err.clear();
    if (w <= 0 || h <= 0)
    {
        err = "Invalid image dimensions.";
        return false;
    }
    if ((int)indices.size() < w * h)
    {
        err = "Invalid indexed buffer size.";
        return false;
    }
    if (palette_rgba.empty() || (palette_rgba.size() % 4u) != 0u)
    {
        err = "Invalid palette buffer.";
        return false;
    }
    if (!(bitdepth == 4 || bitdepth == 8))
    {
        err = "Unsupported indexed PNG bit depth.";
        return false;
    }

    LodePNGState state;
    lodepng_state_init(&state);

    state.info_png.color.colortype = LCT_PALETTE;
    state.info_png.color.bitdepth = (unsigned)bitdepth;
    state.info_raw.colortype = LCT_PALETTE;
    state.info_raw.bitdepth = (unsigned)bitdepth;

    ConfigurePngCompression(state, compression);

    const size_t pal_n = palette_rgba.size() / 4u;
    for (size_t i = 0; i < pal_n; ++i)
    {
        const std::uint8_t r = palette_rgba[i * 4u + 0];
        const std::uint8_t g = palette_rgba[i * 4u + 1];
        const std::uint8_t b = palette_rgba[i * 4u + 2];
        const std::uint8_t a = palette_rgba[i * 4u + 3];
        lodepng_palette_add(&state.info_png.color, r, g, b, a);
        lodepng_palette_add(&state.info_raw, r, g, b, a);
    }

    std::vector<std::uint8_t> packed;
    const std::uint8_t* src = indices.data();
    const std::uint8_t* raw = nullptr;

    if (bitdepth == 8)
    {
        raw = src;
    }
    else
    {
        // Pack 2 pixels per byte, MS nibble first, row by row.
        const size_t row_bytes = ((size_t)w + 1u) / 2u;
        packed.assign(row_bytes * (size_t)h, 0u);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; x += 2)
            {
                const std::uint8_t i0 = (std::uint8_t)(src[(size_t)y * (size_t)w + (size_t)x] & 0x0F);
                const std::uint8_t i1 = (x + 1 < w) ? (std::uint8_t)(src[(size_t)y * (size_t)w + (size_t)(x + 1)] & 0x0F) : 0;
                const std::uint8_t byte = (std::uint8_t)((i0 << 4) | (i1));
                packed[(size_t)y * row_bytes + (size_t)(x / 2)] = byte;
            }
        }
        raw = packed.data();
    }

    unsigned char* out = nullptr;
    size_t out_size = 0;
    const unsigned enc_err = lodepng_encode(&out, &out_size, raw, (unsigned)w, (unsigned)h, &state);
    if (enc_err != 0)
    {
        err = std::string("lodepng_encode failed: ") + lodepng_error_text(enc_err);
        lodepng_state_cleanup(&state);
        return false;
    }

    const unsigned save_err = lodepng_save_file(out, out_size, path.c_str());
    free(out);
    lodepng_state_cleanup(&state);
    if (save_err != 0)
    {
        err = std::string("lodepng_save_file failed: ") + lodepng_error_text(save_err);
        return false;
    }
    return true;
}

static bool WritePngTruecolor(const std::string& path,
                              int w,
                              int h,
                              const std::vector<std::uint8_t>& rgba,
                              bool with_alpha,
                              int compression,
                              std::string& err)
{
    err.clear();
    if (w <= 0 || h <= 0)
    {
        err = "Invalid image dimensions.";
        return false;
    }
    const size_t need = (size_t)w * (size_t)h * 4u;
    if (rgba.size() < need)
    {
        err = "Invalid RGBA buffer size.";
        return false;
    }

    // Use lodepng's convenience API for truecolor paths.
    if (with_alpha)
    {
        LodePNGState state;
        lodepng_state_init(&state);
        state.info_raw.colortype = LCT_RGBA;
        state.info_raw.bitdepth = 8;
        state.info_png.color.colortype = LCT_RGBA;
        state.info_png.color.bitdepth = 8;
        ConfigurePngCompression(state, compression);

        unsigned char* out = nullptr;
        size_t out_size = 0;
        const unsigned enc_err = lodepng_encode(&out, &out_size, rgba.data(), (unsigned)w, (unsigned)h, &state);
        if (enc_err != 0)
        {
            err = std::string("lodepng_encode failed: ") + lodepng_error_text(enc_err);
            lodepng_state_cleanup(&state);
            return false;
        }
        const unsigned save_err = lodepng_save_file(out, out_size, path.c_str());
        free(out);
        lodepng_state_cleanup(&state);
        if (save_err != 0)
        {
            err = std::string("lodepng_save_file failed: ") + lodepng_error_text(save_err);
            return false;
        }
        return true;
    }
    else
    {
        // RGB (drop alpha).
        std::vector<std::uint8_t> rgb;
        rgb.resize((size_t)w * (size_t)h * 3u);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t si = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                const size_t di = ((size_t)y * (size_t)w + (size_t)x) * 3u;
                rgb[di + 0] = rgba[si + 0];
                rgb[di + 1] = rgba[si + 1];
                rgb[di + 2] = rgba[si + 2];
            }
        }

        LodePNGState state;
        lodepng_state_init(&state);
        state.info_raw.colortype = LCT_RGB;
        state.info_raw.bitdepth = 8;
        state.info_png.color.colortype = LCT_RGB;
        state.info_png.color.bitdepth = 8;
        ConfigurePngCompression(state, compression);

        unsigned char* out = nullptr;
        size_t out_size = 0;
        const unsigned enc_err = lodepng_encode(&out, &out_size, rgb.data(), (unsigned)w, (unsigned)h, &state);
        if (enc_err != 0)
        {
            err = std::string("lodepng_encode failed: ") + lodepng_error_text(enc_err);
            lodepng_state_cleanup(&state);
            return false;
        }
        const unsigned save_err = lodepng_save_file(out, out_size, path.c_str());
        free(out);
        lodepng_state_cleanup(&state);
        if (save_err != 0)
        {
            err = std::string("lodepng_save_file failed: ") + lodepng_error_text(save_err);
            return false;
        }
        return true;
    }
}
} // namespace

const std::vector<std::string_view>& ImportExtensions()
{
    static const std::vector<std::string_view> exts = {"png", "jpg", "jpeg", "gif", "bmp"};
    return exts;
}

const std::vector<std::string_view>& ExportExtensions()
{
    static const std::vector<std::string_view> exts = {"png", "jpg", "jpeg"};
    return exts;
}

bool ComputeExportDimensionsPx(const AnsiCanvas& canvas,
                               int& out_w,
                               int& out_h,
                               std::string& err,
                               const ExportOptions& options)
{
    canvas_rasterizer::Options ropt;
    ropt.scale = options.scale;
    ropt.transparent_unset_bg = options.transparent_unset_bg;
    return canvas_rasterizer::ComputeCompositeRasterSize(canvas, out_w, out_h, err, ropt);
}

bool ImportFileToRgba(const std::string& path, RgbaImage& out, std::string& err)
{
    err.clear();
    out = {};

    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!image_loader::LoadImageAsRgba32(path, w, h, rgba, err))
        return false;
    out.width = w;
    out.height = h;
    out.pixels.assign(rgba.begin(), rgba.end());
    return true;
}

bool ExportCanvasToFile(const std::string& path,
                        const AnsiCanvas& canvas,
                        std::string& err,
                        const ExportOptions& options)
{
    err.clear();

    const std::string ext = FileExtLower(path);
    if (ext.empty())
    {
        err = "Missing file extension.";
        return false;
    }

    canvas_rasterizer::Options ropt;
    ropt.scale = options.scale;
    ropt.transparent_unset_bg = options.transparent_unset_bg;

    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    std::string rerr;
    if (!canvas_rasterizer::RasterizeCompositeToRgba32(canvas, rgba, w, h, rerr, ropt))
    {
        err = rerr.empty() ? "Rasterize failed." : rerr;
        return false;
    }

    if (ext == "jpg" || ext == "jpeg")
    {
        if (options.transparent_unset_bg)
        {
            err = "JPEG does not support transparency. Export as PNG (32-bit) instead.";
            return false;
        }
        return image_writer::WriteJpgFromRgba32(path, w, h, rgba, options.jpg_quality, err);
    }
    else if (ext == "png")
    {
        const ExportOptions::PngFormat mode = options.png_format;

        if (options.transparent_unset_bg &&
            (mode == ExportOptions::PngFormat::Rgb24 || mode == ExportOptions::PngFormat::Indexed4 ||
             mode == ExportOptions::PngFormat::Indexed8))
        {
            err = "Selected PNG format does not support transparency. Use PNG RGBA (32-bit) instead.";
            return false;
        }

        if (mode == ExportOptions::PngFormat::Rgb24)
        {
            return WritePngTruecolor(path, w, h, rgba, /*with_alpha=*/false, options.png_compression, err);
        }
        if (mode == ExportOptions::PngFormat::Rgba32)
        {
            return WritePngTruecolor(path, w, h, rgba, /*with_alpha=*/true, options.png_compression, err);
        }
        if (mode == ExportOptions::PngFormat::Indexed8)
        {
            std::vector<std::uint8_t> idx;
            std::vector<std::uint8_t> pal;
            if (options.xterm_240_safe)
            {
                QuantizeToXterm240Safe(rgba, w, h, idx, pal);
                return WritePngIndexed(path, w, h, idx, pal, /*bitdepth=*/8, options.png_compression, err);
            }
            else
            {
                QuantizeToXterm256(rgba, w, h, idx, pal);
                return WritePngIndexed(path, w, h, idx, pal, /*bitdepth=*/8, options.png_compression, err);
            }
        }
        if (mode == ExportOptions::PngFormat::Indexed4)
        {
            std::vector<std::uint8_t> idx;
            std::vector<std::uint8_t> pal;
            QuantizeToXterm16(rgba, w, h, idx, pal);
            return WritePngIndexed(path, w, h, idx, pal, /*bitdepth=*/4, options.png_compression, err);
        }
        err = "Unsupported PNG format option.";
        return false;
    }

    err = "Unsupported image format for export.";
    return false;
}
} // namespace image
} // namespace formats


