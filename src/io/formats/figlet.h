#pragma once

#include "core/canvas.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace formats::figlet
{
const std::vector<std::string_view>& ImportExtensions(); // {"flf"}
const std::vector<std::string_view>& ExportExtensions(); // empty

struct ImportOptions
{
    // Text to render into the canvas (MVP).
    std::string text = "PHOSPHOR";

    // textmode font render behavior:
    bool edit_mode = false;
    int outline_style = 0;

    // If true, apply rendered per-cell colours (FIGlet is usually monochrome, so this is typically false/unused).
    bool use_font_colours = false;

    // If true, treat blink bit as bright background (ICE colours) when applicable.
    bool icecolours = true;
};

bool ImportBytesToCanvas(const std::vector<std::uint8_t>& bytes,
                         AnsiCanvas& out_canvas,
                         std::string& err,
                         const ImportOptions& options = {});

bool ImportFileToCanvas(const std::string& path,
                        AnsiCanvas& out_canvas,
                        std::string& err,
                        const ImportOptions& options = {});
} // namespace formats::figlet


