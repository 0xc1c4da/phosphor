// Phase 3: bridge between AnsiCanvas storage and CRDT tile schema (tile_fmt=1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "net/p2p/canvas_tile_schema_v1.h"
#include "net/p2p/tile_codec_v1.h"

namespace phos::p2p
{
// Encode a tile from an AnsiCanvas layer into tile_fmt=1 bytes.
bool EncodeCanvasTileV1(const AnsiCanvas& canvas,
                        std::size_t layer_index,
                        std::size_t tile_size,
                        std::int64_t ty,
                        std::int64_t tx,
                        std::vector<std::uint8_t>& out_tile_bytes,
                        std::string* err = nullptr);

// Apply tile_fmt=1 bytes into an AnsiCanvas layer at tile coordinate (ty, tx).
bool ApplyCanvasTileV1(AnsiCanvas& canvas,
                       std::size_t layer_index,
                       std::size_t tile_size,
                       std::int64_t ty,
                       std::int64_t tx,
                       std::span<const std::uint8_t> tile_bytes,
                       std::string* err = nullptr);

// Convenience: encode from canvas then write into CRDT schema at tiles["ty,tx"].
bool PutCanvasTileV1(AutomergeAdapter& doc,
                     const AnsiCanvas& canvas,
                     std::size_t layer_index,
                     std::size_t tile_size,
                     std::int64_t ty,
                     std::int64_t tx,
                     std::string* err = nullptr);

// Convenience: read CRDT tile bytes and apply into the canvas. Returns false on schema/codec errors.
bool ApplyDocTileToCanvasV1(AnsiCanvas& canvas,
                            const AutomergeAdapter& doc,
                            std::size_t layer_index,
                            std::size_t tile_size,
                            std::int64_t ty,
                            std::int64_t tx,
                            std::string* err = nullptr);
} // namespace phos::p2p


