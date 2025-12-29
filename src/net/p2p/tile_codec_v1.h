// Tile codec for multiplayer CRDT tiles (tile_fmt = 1).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace phos::p2p
{
struct TileCellV1
{
    std::uint32_t g = 0;    // GlyphId token
    std::uint16_t fg = 0;   // ColourIndex16
    std::uint16_t bg = 0;   // ColourIndex16
    std::uint16_t a = 0;    // Attrs (low 16 bits)
};

// tile_fmt=1 constants.
inline constexpr std::size_t kTileCellBytesV1 = 10;

inline std::size_t TileByteLenV1(std::size_t tile_size) { return tile_size * tile_size * kTileCellBytesV1; }

// Encodes tile_size*tile_size cells (row-major) into bytes. Returns false on invalid input size.
bool EncodeTileV1(std::size_t tile_size, const std::vector<TileCellV1>& cells, std::vector<std::uint8_t>& out_bytes,
                  std::string* err = nullptr);

// Decodes bytes into tile_size*tile_size cells (row-major). Returns false if length mismatch.
bool DecodeTileV1(std::size_t tile_size, const std::vector<std::uint8_t>& bytes, std::vector<TileCellV1>& out_cells,
                  std::string* err = nullptr);
} // namespace phos::p2p


