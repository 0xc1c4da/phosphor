#pragma once

#include "core/lut/lut_cache.h"
#include "core/colour_ops.h"
#include "core/palette/palette.h"
#include "core/palette/palette_catalog.h"

namespace phos::colour
{
// Integration-level singleton for the initial refactor landing.
// Longer-term, this should become an owned service in AppState/SessionState and threaded through.
class ColourSystem
{
public:
    PaletteRegistry& Palettes() { return m_palettes; }
    const PaletteRegistry& Palettes() const { return m_palettes; }

    PaletteCatalog& Catalog() { return m_catalog; }
    const PaletteCatalog& Catalog() const { return m_catalog; }

    LutCache& Luts() { return m_luts; }
    const LutCache& Luts() const { return m_luts; }

    ColourOps& Ops() { return m_ops; }
    const ColourOps& Ops() const { return m_ops; }

private:
    PaletteRegistry m_palettes;
    PaletteCatalog m_catalog;
    LutCache m_luts;
    ColourOps m_ops;
};

ColourSystem& GetColourSystem();

} // namespace phos::colour


