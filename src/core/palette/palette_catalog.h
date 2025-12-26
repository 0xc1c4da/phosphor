#pragma once

#include "core/palette/palette.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace phos::colour
{
// PaletteCatalog is a presentation/config layer on top of PaletteRegistry:
// - Builtins are always available and always listed (stable ordering).
// - Optional external palettes (e.g. assets/colour-palettes.json) are loaded and registered
//   as dynamic palettes in the registry.
//
// The registry remains the single source of truth for palette RGB tables; the catalog
// provides a stable UI ordering and optional grouping decisions.
class PaletteCatalog
{
public:
    PaletteCatalog() = default;

    // Loads palettes from a JSON catalog file and rebuilds the catalog list.
    // This does NOT clear palettes from the registry (registry is canonical and may contain
    // palettes referenced by projects/imports). It only rebuilds the catalog's UI list.
    //
    // Returns true if the file was successfully loaded and parsed; returns false on error.
    // On error, builtins remain available via UiPaletteList().
    bool LoadFromJsonFile(const std::string& path, std::string& out_error);

    // Appends a palette to a JSON catalog file (typically assets/colour-palettes.json).
    // - Creates the file if it does not exist.
    // - Ensures a unique "title" within the JSON (appends " (n)" suffix if needed).
    //
    // Returns true on success; returns false on failure and sets out_error.
    // If out_final_title is non-null, it is set to the title actually written.
    bool AppendToJsonFile(const std::string& path,
                          std::string_view wanted_title,
                          std::span<const Rgb8> rgb,
                          std::string& out_error,
                          std::string* out_final_title = nullptr);

    // Current UI palette list in stable order.
    // Includes builtins first, then any loaded catalog palettes.
    std::span<const PaletteInstanceId> UiPaletteList() const { return m_ui_list; }

    // Convenience: find a palette instance id in UiPaletteList() by PaletteRef.
    std::optional<PaletteInstanceId> FindInUiListByRef(const PaletteRef& ref) const;

    // Ensure the given ref is present in UiPaletteList(). If it resolves in the registry and is not
    // currently listed, it is appended to the end of the UI list. Returns the resolved instance id
    // on success, or nullopt if the ref cannot be resolved.
    std::optional<PaletteInstanceId> EnsureUiIncludes(const PaletteRef& ref);

    // Best-match inference helpers for importers.
    //
    // These return a PaletteRef from the current UiPaletteList() when a confident match is found.
    // If no confident match exists, they return nullopt (callers should fall back to "follow core palette").
    //
    // - BestMatchUiByIndexOrder(): compares an explicit palette table against candidates of the same size.
    //   This is intended for formats that carry a palette table (e.g. XBin). Palette index order matters.
    //
    // - BestMatchUiByNearestColours(): compares a set of observed RGB colours against each candidate palette,
    //   scoring by nearest-neighbor distance (order does not matter). This is intended for formats that
    //   don't carry a palette table but do carry explicit RGB colours (e.g. truecolour ANSI sequences).
    std::optional<PaletteRef> BestMatchUiByIndexOrder(std::span<const Rgb8> table_rgb) const;
    std::optional<PaletteRef> BestMatchUiByNearestColours(std::span<const Rgb8> colours) const;

    // Optional load error message (empty when last load was successful).
    const std::string& LastLoadError() const { return m_last_error; }

private:
    void RebuildBuiltinList();
    void AppendCatalogPalette(PaletteInstanceId id);

    std::vector<PaletteInstanceId> m_ui_list;
    std::vector<PaletteInstanceId> m_catalog_only; // dynamic palettes sourced from JSON catalog (for diagnostics)
    std::string m_last_error;
};

} // namespace phos::colour


