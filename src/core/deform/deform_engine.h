#pragma once

#include "core/canvas.h"

#include <optional>
#include <string>
#include <vector>

namespace deform
{
enum class Mode : std::uint8_t
{
    Move = 0,
    Grow,
    Shrink,
    SwirlCw,
    SwirlCcw,
};

enum class Sample : std::uint8_t
{
    Layer = 0,
    Composite,
};

enum class DeformAlgo : std::uint8_t
{
    WarpQuantize = 0,       // rasterize -> warp -> quantize (original)
    WarpQuantizeSticky,     // warp -> quantize, but strongly bias towards source/region glyphs
    CellResample,           // cell-domain inverse-map + copy from source snapshot (preserve glyph identities)
};

enum class GlyphSetKind : std::uint8_t
{
    FontAll = 0,    // use all glyphs available in the current font (bitmap fonts: 256/512)
    Ascii,          // printable ASCII
    BasicBlocks,    // space + common block elements
    ExplicitList,   // use explicit_codepoints
};

struct GlyphSet
{
    GlyphSetKind kind = GlyphSetKind::BasicBlocks;
    // Used when kind == ExplicitList:
    // - explicit_glyph_ids: preferred lossless GlyphId tokens (may be UnicodeScalar or token-space)
    // - explicit_codepoints: legacy Unicode-only list
    std::vector<AnsiCanvas::GlyphId> explicit_glyph_ids;
    std::vector<char32_t> explicit_codepoints;
};

struct ApplyDabArgs
{
    // Center in canvas cell coordinates.
    float x = 0.0f;
    float y = 0.0f;

    // Previous center (required for Move).
    std::optional<float> prev_x;
    std::optional<float> prev_y;

    // Brush diameter in cells (>=1).
    int size = 1;

    // 0..1
    float hardness = 0.8f;
    float strength = 1.0f;

    // Behavior.
    Mode mode = Mode::Move;
    // Algorithm for all modes.
    DeformAlgo algo = DeformAlgo::WarpQuantize;
    // Optional additional intensity knob (meaning depends on mode):
    // - Swirl: scales theta_max
    // - Grow/Shrink: scales the signed scale factor
    float amount = 1.0f;
    Sample sample = Sample::Layer;

    // Clip region in *cell* coordinates. If empty (w/h <= 0), the engine will use full canvas bounds.
    AnsiCanvas::Rect clip = {};

    // Active palette identity for the canvas (used for snapping/quantization).
    // Default is xterm256 to preserve current behavior.
    phos::colour::PaletteRef palette_ref = []{
        phos::colour::PaletteRef r;
        r.is_builtin = true;
        r.builtin = phos::colour::BuiltinPalette::Xterm256;
        return r;
    }();

    // Optional restriction: allowed indices (in the active palette index space).
    // If provided, colour snapping should choose from these.
    const std::vector<int>* allowed_indices = nullptr;

    // Candidate glyph set.
    GlyphSet glyph_set = {};

    // Stability: if > 0, prefer keeping the existing glyph when it is "close enough".
    float hysteresis = 0.0f;
};

struct ApplyDabResult
{
    bool changed = false;
    // Affected region in cell coordinates (for minimal redraw).
    AnsiCanvas::Rect affected = {};
};

// Stateless v1 engine. Heavy work will be implemented in the .cpp (rasterize -> warp -> quantize).
class DeformEngine
{
public:
    ApplyDabResult ApplyDab(AnsiCanvas& canvas,
                            int layer_index,
                            const ApplyDabArgs& args,
                            std::string& err) const;
};
} // namespace deform


