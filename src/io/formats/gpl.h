#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// GIMP Palette format (.gpl) importer.
//
// Spec sketch:
//   GIMP Palette
//   Name: My Palette
//   Columns: 16
//   # comment lines...
//   R G B [optional name...]
//
// References:
// - https://developer.gimp.org/core/standards/gpl/
namespace formats::gpl
{
// Lowercase extension (no leading dot).
const std::vector<std::string_view>& ImportExtensions(); // {"gpl"}

struct Colour
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::string  name; // optional
};

struct Palette
{
    std::string        name;    // from "Name:" header (or caller-provided fallback)
    int                columns = 0;
    std::vector<Colour> colours;
};

bool ImportBytesToPalette(const std::vector<std::uint8_t>& bytes,
                          Palette& out_palette,
                          std::string& err,
                          std::string_view fallback_name = {});

bool ImportFileToPalette(const std::string& path,
                         Palette& out_palette,
                         std::string& err,
                         std::string_view fallback_name = {});
} // namespace formats::gpl


