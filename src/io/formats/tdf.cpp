#include "io/formats/tdf.h"

#include "core/color_system.h"
#include "fonts/textmode_font.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace formats::tdf
{
const std::vector<std::string_view>& ImportExtensions()
{
    static const std::vector<std::string_view> exts = {"tdf"};
    return exts;
}

const std::vector<std::string_view>& ExportExtensions()
{
    static const std::vector<std::string_view> exts = {};
    return exts;
}

namespace
{
static phos::color::BuiltinPalette ChooseBuiltinPaletteForBitmap(const textmode_font::Bitmap& bmp)
{
    // If the bitmap uses colors and they all match VGA16 exactly, keep the canvas palette as VGA16.
    // Otherwise quantize into xterm256 (fallback).
    auto& cs = phos::color::GetColorSystem();
    const auto pal_vga16 = cs.Palettes().Builtin(phos::color::BuiltinPalette::Vga16);

    const phos::color::Palette* vga = cs.Palettes().Get(pal_vga16);
    auto is_in_vga16 = [&](std::uint32_t c32) -> bool {
        if (c32 == 0)
            return true; // unset
        if (!vga || vga->rgb.empty())
            return false;
        std::uint8_t r = 0, g = 0, b = 0;
        if (!phos::color::ColorOps::UnpackImGuiAbgr(c32, r, g, b))
            return true; // treat non-color as unset
        const std::uint32_t u24 = (std::uint32_t)r | ((std::uint32_t)g << 8) | ((std::uint32_t)b << 16);
        return vga->exact_u24_to_index.find(u24) != vga->exact_u24_to_index.end();
    };

    bool any_color = false;
    for (std::uint32_t c : bmp.fg)
    {
        if (c != 0) any_color = true;
        if (!is_in_vga16(c))
            return phos::color::BuiltinPalette::Xterm256;
    }
    for (std::uint32_t c : bmp.bg)
    {
        if (c != 0) any_color = true;
        if (!is_in_vga16(c))
            return phos::color::BuiltinPalette::Xterm256;
    }
    return any_color ? phos::color::BuiltinPalette::Vga16 : phos::color::BuiltinPalette::Xterm256;
}

static std::vector<AnsiCanvas::ColorIndex16> QuantizeColor32VectorToIndices(const std::vector<std::uint32_t>& in,
                                                                           phos::color::PaletteInstanceId pal)
{
    auto& cs = phos::color::GetColorSystem();
    const phos::color::QuantizePolicy qp = phos::color::DefaultQuantizePolicy();
    std::vector<AnsiCanvas::ColorIndex16> out;
    out.resize(in.size(), AnsiCanvas::kUnsetIndex16);
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const phos::color::ColorIndex idx = phos::color::ColorOps::Color32ToIndex(cs.Palettes(), pal, in[i], qp);
        out[i] = idx.IsUnset() ? AnsiCanvas::kUnsetIndex16 : (AnsiCanvas::ColorIndex16)idx.v;
    }
    return out;
}

static std::vector<std::uint8_t> ReadAllBytes(const std::string& path, std::string& err)
{
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "Failed to open file for reading.";
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    if (sz < 0)
    {
        err = "Failed to read file size.";
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out;
    out.resize((size_t)sz);
    if (sz > 0)
        in.read(reinterpret_cast<char*>(out.data()), sz);
    if (!in && sz > 0)
    {
        err = "Failed to read file contents.";
        return {};
    }
    return out;
}
} // namespace

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options)
{
    err.clear();

    std::vector<textmode_font::Font> fonts;
    std::string ferr;
    if (!textmode_font::LoadFontsFromBytes(bytes, fonts, ferr))
    {
        err = ferr.empty() ? "Failed to parse TDF font." : ferr;
        return false;
    }
    if (fonts.empty())
    {
        err = "TDF: bundle contains no fonts.";
        return false;
    }
    if (textmode_font::GetMeta(fonts[0]).kind != textmode_font::Kind::Tdf)
    {
        err = "Not a TDF font.";
        return false;
    }

    const int idx = std::clamp(options.bundle_index, 0, (int)fonts.size() - 1);
    const textmode_font::Font& font = fonts[(size_t)idx];
    const textmode_font::FontMeta meta = textmode_font::GetMeta(font);

    textmode_font::RenderOptions ro;
    ro.mode = options.edit_mode ? textmode_font::RenderMode::Edit : textmode_font::RenderMode::Display;
    ro.outline_style = options.outline_style;
    ro.use_font_colors = options.use_font_colors && (meta.tdf_type == textmode_font::TdfFontType::Color);
    ro.icecolors = options.icecolors;

    textmode_font::Bitmap bmp;
    if (!textmode_font::RenderText(font, options.text, ro, bmp, ferr))
    {
        err = ferr.empty() ? "TDF render failed." : ferr;
        return false;
    }

    const int cols = std::max(1, bmp.w);
    const int rows = std::max(1, bmp.h);

    AnsiCanvas::ProjectState st;
    st.version = 8;
    st.current.columns = cols;
    st.current.rows = rows;
    st.current.active_layer = 0;
    st.current.caret_row = 0;
    st.current.caret_col = 0;
    st.current.layers.clear();
    st.current.layers.resize(1);
    st.current.layers[0].name = "Base";
    st.current.layers[0].visible = true;
    st.current.layers[0].cells = bmp.cp;

    // Palette identity + indexed colors (Phase B).
    const phos::color::BuiltinPalette builtin = ChooseBuiltinPaletteForBitmap(bmp);
    st.palette_ref.is_builtin = true;
    st.palette_ref.builtin = builtin;
    {
        auto& cs = phos::color::GetColorSystem();
        const phos::color::PaletteInstanceId pal = cs.Palettes().Builtin(builtin);
        st.current.layers[0].fg = QuantizeColor32VectorToIndices(bmp.fg, pal);
        st.current.layers[0].bg = QuantizeColor32VectorToIndices(bmp.bg, pal);
    }

    // Minimal SAUCE record to preserve geometry (and hint at font family).
    st.sauce.present = true;
    st.sauce.data_type = 1; // Character
    st.sauce.file_type = 1; // ANSi
    st.sauce.tinfo1 = (std::uint16_t)std::clamp(cols, 0, 65535);
    st.sauce.tinfo2 = (std::uint16_t)std::clamp(rows, 0, 65535);
    st.sauce.tinfos = meta.name.empty() ? "TheDraw" : meta.name;

    AnsiCanvas canvas(cols);
    std::string apply_err;
    if (!canvas.SetProjectState(st, apply_err))
    {
        err = apply_err.empty() ? "Failed to apply imported TDF state." : apply_err;
        return false;
    }

    out_canvas = std::move(canvas);
    return true;
}

bool ImportFileToCanvas(const std::string& path,
                        AnsiCanvas& out_canvas,
                        std::string& err,
                        const ImportOptions& options)
{
    err.clear();
    std::string rerr;
    const auto bytes = ReadAllBytes(path, rerr);
    if (!rerr.empty())
    {
        err = rerr;
        return false;
    }
    return ImportBytesToCanvas(bytes, out_canvas, err, options);
}
} // namespace formats::tdf


