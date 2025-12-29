// Phase 3: bridge between AnsiCanvas storage and CRDT tile schema (tile_fmt=1).
#include "canvas_multiplayer_bridge.h"

#include <limits>

namespace phos::p2p
{
namespace
{
bool ValidateTileParams(std::size_t tile_size, std::int64_t ty, std::int64_t tx, std::string* err)
{
    if (tile_size == 0)
    {
        if (err)
            *err = "tile_size must be > 0";
        return false;
    }
    // Defensive sanity: avoid pathological allocations on malformed remote input.
    if (tile_size > 1024)
    {
        if (err)
            *err = "tile_size too large";
        return false;
    }
    if (ty < 0 || tx < 0)
    {
        if (err)
            *err = "negative tile coordinate";
        return false;
    }
    return true;
}
} // namespace

bool EncodeCanvasTileV1(const AnsiCanvas& canvas,
                        std::size_t layer_index,
                        std::size_t tile_size,
                        std::int64_t ty,
                        std::int64_t tx,
                        std::vector<std::uint8_t>& out_tile_bytes,
                        std::string* err)
{
    if (!ValidateTileParams(tile_size, ty, tx, err))
        return false;

    const std::size_t cell_count = tile_size * tile_size;
    if (tile_size != 0 && cell_count / tile_size != tile_size)
    {
        if (err)
            *err = "tile_size overflow";
        return false;
    }

    std::vector<TileCellV1> cells;
    cells.resize(cell_count);

    for (std::size_t ly = 0; ly < tile_size; ++ly)
    {
        for (std::size_t lx = 0; lx < tile_size; ++lx)
        {
            const std::size_t i = ly * tile_size + lx;

            const std::int64_t row64 = ty * (std::int64_t)tile_size + (std::int64_t)ly;
            const std::int64_t col64 = tx * (std::int64_t)tile_size + (std::int64_t)lx;
            if (row64 < (std::int64_t)std::numeric_limits<int>::min() || row64 > (std::int64_t)std::numeric_limits<int>::max() ||
                col64 < (std::int64_t)std::numeric_limits<int>::min() || col64 > (std::int64_t)std::numeric_limits<int>::max())
            {
                // Treat out-of-range coordinates as default cells.
                continue;
            }
            const int row = (int)row64;
            const int col = (int)col64;

            TileCellV1 c{};
            c.g = static_cast<std::uint32_t>(canvas.GetLayerGlyph((int)layer_index, row, col));

            AnsiCanvas::ColourIndex16 fg = AnsiCanvas::kUnsetIndex16;
            AnsiCanvas::ColourIndex16 bg = AnsiCanvas::kUnsetIndex16;
            (void)canvas.GetLayerCellIndices((int)layer_index, row, col, fg, bg);
            c.fg = static_cast<std::uint16_t>(fg);
            c.bg = static_cast<std::uint16_t>(bg);

            AnsiCanvas::Attrs attrs = 0;
            (void)canvas.GetLayerCellAttrs((int)layer_index, row, col, attrs);
            c.a = static_cast<std::uint16_t>(attrs);

            cells[i] = c;
        }
    }

    return EncodeTileV1(tile_size, cells, out_tile_bytes, err);
}

bool ApplyCanvasTileV1(AnsiCanvas& canvas,
                       std::size_t layer_index,
                       std::size_t tile_size,
                       std::int64_t ty,
                       std::int64_t tx,
                       std::span<const std::uint8_t> tile_bytes,
                       std::string* err)
{
    if (!ValidateTileParams(tile_size, ty, tx, err))
        return false;

    std::vector<std::uint8_t> bytes_vec(tile_bytes.begin(), tile_bytes.end());
    std::vector<TileCellV1> cells;
    if (!DecodeTileV1(tile_size, bytes_vec, cells, err))
        return false;

    const int cols = canvas.GetColumns();
    for (std::size_t ly = 0; ly < tile_size; ++ly)
    {
        for (std::size_t lx = 0; lx < tile_size; ++lx)
        {
            const std::size_t i = ly * tile_size + lx;
            if (i >= cells.size())
                break;

            const std::int64_t row64 = ty * (std::int64_t)tile_size + (std::int64_t)ly;
            const std::int64_t col64 = tx * (std::int64_t)tile_size + (std::int64_t)lx;
            if (row64 < 0 || col64 < 0)
                continue;
            if (row64 > (std::int64_t)std::numeric_limits<int>::max() || col64 > (std::int64_t)std::numeric_limits<int>::max())
                continue;

            const int row = (int)row64;
            const int col = (int)col64;
            if (col >= cols)
                continue;

            const TileCellV1& c = cells[i];
            const auto glyph = static_cast<AnsiCanvas::GlyphId>(c.g);
            const auto fg = static_cast<AnsiCanvas::ColourIndex16>(c.fg);
            const auto bg = static_cast<AnsiCanvas::ColourIndex16>(c.bg);
            const auto attrs = static_cast<AnsiCanvas::Attrs>(c.a);

            // Note: this call grows rows as needed.
            if (!canvas.SetLayerGlyphIndicesPartial((int)layer_index, row, col, glyph, fg, bg, attrs))
            {
                if (err)
                    *err = "ApplyCanvasTileV1: SetLayerGlyphIndicesPartial failed";
                return false;
            }
        }
    }

    return true;
}

bool PutCanvasTileV1(AutomergeAdapter& doc,
                     const AnsiCanvas& canvas,
                     std::size_t layer_index,
                     std::size_t tile_size,
                     std::int64_t ty,
                     std::int64_t tx,
                     std::string* err)
{
    std::vector<std::uint8_t> bytes;
    if (!EncodeCanvasTileV1(canvas, layer_index, tile_size, ty, tx, bytes, err))
        return false;
    return PutTileV1(doc, layer_index, ty, tx, bytes, err);
}

bool ApplyDocTileToCanvasV1(AnsiCanvas& canvas,
                            const AutomergeAdapter& doc,
                            std::size_t layer_index,
                            std::size_t tile_size,
                            std::int64_t ty,
                            std::int64_t tx,
                            std::string* err)
{
    auto bytes = GetTileV1(doc, layer_index, ty, tx, err);
    if (!bytes)
    {
        if (err && err->empty())
            *err = "ApplyDocTileToCanvasV1: missing tile";
        return false;
    }
    return ApplyCanvasTileV1(canvas, layer_index, tile_size, ty, tx, *bytes, err);
}
} // namespace phos::p2p


