#pragma once

#include "core/lut/lut_cache.h"
#include "core/color_ops.h"
#include "core/palette/palette.h"
#include "core/palette/palette_catalog.h"

namespace phos::color
{
// Integration-level singleton for the initial refactor landing.
// Longer-term, this should become an owned service in AppState/SessionState and threaded through.
class ColorSystem
{
public:
    PaletteRegistry& Palettes() { return m_palettes; }
    const PaletteRegistry& Palettes() const { return m_palettes; }

    PaletteCatalog& Catalog() { return m_catalog; }
    const PaletteCatalog& Catalog() const { return m_catalog; }

    LutCache& Luts() { return m_luts; }
    const LutCache& Luts() const { return m_luts; }

    ColorOps& Ops() { return m_ops; }
    const ColorOps& Ops() const { return m_ops; }

private:
    PaletteRegistry m_palettes;
    PaletteCatalog m_catalog;
    LutCache m_luts;
    ColorOps m_ops;
};

ColorSystem& GetColorSystem();

} // namespace phos::color


