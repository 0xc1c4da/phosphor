#pragma once

#include "core/palette/palette.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phos::color
{
// PaletteCatalog is a presentation/config layer on top of PaletteRegistry:
// - Builtins are always available and always listed (stable ordering).
// - Optional external palettes (e.g. assets/color-palettes.json) are loaded and registered
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

    // Current UI palette list in stable order.
    // Includes builtins first, then any loaded catalog palettes.
    std::span<const PaletteInstanceId> UiPaletteList() const { return m_ui_list; }

    // Convenience: find a palette instance id in UiPaletteList() by PaletteRef.
    std::optional<PaletteInstanceId> FindInUiListByRef(const PaletteRef& ref) const;

    // Ensure the given ref is present in UiPaletteList(). If it resolves in the registry and is not
    // currently listed, it is appended to the end of the UI list. Returns the resolved instance id
    // on success, or nullopt if the ref cannot be resolved.
    std::optional<PaletteInstanceId> EnsureUiIncludes(const PaletteRef& ref);

    // Optional load error message (empty when last load was successful).
    const std::string& LastLoadError() const { return m_last_error; }

private:
    void RebuildBuiltinList();
    void AppendCatalogPalette(PaletteInstanceId id);

    std::vector<PaletteInstanceId> m_ui_list;
    std::vector<PaletteInstanceId> m_catalog_only; // dynamic palettes sourced from JSON catalog (for diagnostics)
    std::string m_last_error;
};

} // namespace phos::color


