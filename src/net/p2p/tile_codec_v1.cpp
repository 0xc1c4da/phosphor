#include "tile_codec_v1.h"

#include "byte_io.h"

#include <span>

namespace phos::p2p
{
bool EncodeTileV1(std::size_t tile_size, const std::vector<TileCellV1>& cells, std::vector<std::uint8_t>& out_bytes,
                  std::string* err)
{
    const std::size_t expected_cells = tile_size * tile_size;
    if (cells.size() != expected_cells)
    {
        if (err)
            *err = "EncodeTileV1: cell count mismatch";
        return false;
    }

    out_bytes.clear();
    out_bytes.reserve(TileByteLenV1(tile_size));
    for (std::size_t i = 0; i < expected_cells; ++i)
    {
        const auto& c = cells[i];
        AppendU32LE(out_bytes, c.g);
        AppendU16LE(out_bytes, c.fg);
        AppendU16LE(out_bytes, c.bg);
        AppendU16LE(out_bytes, c.a);
    }
    return true;
}

bool DecodeTileV1(std::size_t tile_size, const std::vector<std::uint8_t>& bytes, std::vector<TileCellV1>& out_cells,
                  std::string* err)
{
    const std::size_t expected_len = TileByteLenV1(tile_size);
    if (bytes.size() != expected_len)
    {
        if (err)
            *err = "DecodeTileV1: invalid tile length";
        return false;
    }

    const std::size_t expected_cells = tile_size * tile_size;
    out_cells.clear();
    out_cells.resize(expected_cells);

    std::size_t off = 0;
    const std::span<const std::uint8_t> in(bytes.data(), bytes.size());
    try
    {
        for (std::size_t i = 0; i < expected_cells; ++i)
        {
            TileCellV1 c;
            c.g = ReadU32LE(in, off);
            c.fg = ReadU16LE(in, off);
            c.bg = ReadU16LE(in, off);
            c.a = ReadU16LE(in, off);
            out_cells[i] = c;
        }
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = std::string("DecodeTileV1: ") + e.what();
        return false;
    }

    return off == expected_len;
}
} // namespace phos::p2p


