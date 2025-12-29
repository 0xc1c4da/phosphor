// Minimal CRDT schema helpers for Phosphor canvas tile storage (Phase 2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phos::p2p
{
class AutomergeAdapter;

struct CanvasTileSchemaV1Config
{
    std::int64_t schema = 1;
    std::int64_t columns = 0;
    std::int64_t rows = 0;
    std::int64_t tile_size = 16;
    std::int64_t tile_fmt = 1;
};

// Ensures the minimal schema exists:
// root.schema=1, root.canvas.{columns,rows,tile_size,tile_fmt}, root.canvas.layers[0].tiles (map).
bool EnsureCanvasTileSchemaV1(AutomergeAdapter& doc, const CanvasTileSchemaV1Config& cfg, std::string* err = nullptr);

// Writes layer_index/ty/tx tile bytes into canvas.layers[layer_index].tiles["ty,tx"].
bool PutTileV1(AutomergeAdapter& doc, std::size_t layer_index, std::int64_t ty, std::int64_t tx,
               std::span<const std::uint8_t> tile_bytes, std::string* err = nullptr);

// Reads canvas.layers[layer_index].tiles["ty,tx"].
// Returns nullopt if missing or not bytes.
std::optional<std::vector<std::uint8_t>> GetTileV1(const AutomergeAdapter& doc, std::size_t layer_index, std::int64_t ty,
                                                   std::int64_t tx, std::string* err = nullptr);
} // namespace phos::p2p


