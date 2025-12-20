#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace formats::tdf
{
const std::vector<std::string_view>& ImportExtensions(); // {"tdf"}
const std::vector<std::string_view>& ExportExtensions(); // empty

struct ImportOptions
{
    // If the TDF is a bundle, choose which font to use.
    int bundle_index = 0;

    // Text to render into the canvas.
    std::string text = "PHOSPHOR";

    // textmode font render behavior:
    bool edit_mode = false;
    int outline_style = 0;

    // If true and the selected font is Color type, honor its per-cell colors.
    // If false, leave fg/bg unset so callers can stamp their own colors.
    bool use_font_colors = true;

    // If true, treat blink bit as bright background (ICE colors).
    bool icecolors = true;
};

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options = {});

bool ImportFileToCanvas(const std::string& path,
                        AnsiCanvas& out_canvas,
                        std::string& err,
                        const ImportOptions& options = {});
} // namespace formats::tdf


