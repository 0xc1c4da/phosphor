#include "canvas_tile_schema_v1.h"

#include "automerge_adapter.h"

#include <sstream>

namespace phos::p2p
{
namespace
{
inline AMbyteSpan ByteSpan(std::string_view s)
{
    return AMbyteSpan{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

inline AMbyteSpan ByteSpan(std::span<const std::uint8_t> b) { return AMbyteSpan{b.data(), b.size()}; }

inline bool ResultOk(AMresult* r) { return r != nullptr && AMresultStatus(r) == AM_STATUS_OK; }

inline std::string ResultErrorStr(AMresult* r)
{
    if (!r)
        return "null result";
    if (AMresultStatus(r) == AM_STATUS_OK)
        return {};
    const AMbyteSpan s = ::AMresultError(r);
    return std::string(reinterpret_cast<const char*>(s.src), s.count);
}

const AMobjId* GetOrCreateMap(AMdoc* doc, AMstack** stack, const AMobjId* parent, std::string_view key, std::string* err)
{
    AMresult* get_r = AMmapGet(doc, parent, ByteSpan(key), nullptr);
    AMitem* get_it = AMstackItem(stack, get_r, nullptr, nullptr);
    if (!ResultOk(get_r) || !get_it)
    {
        if (err)
            *err = "AMmapGet(" + std::string(key) + ") failed: " + ResultErrorStr(get_r);
        return nullptr;
    }

    if (AMitemValType(get_it) != AM_VAL_TYPE_VOID)
    {
        if (const AMobjId* oid = AMitemObjId(get_it))
            return oid;
    }

    AMresult* put_r = AMmapPutObject(doc, parent, ByteSpan(key), AM_OBJ_TYPE_MAP);
    AMitem* put_it = AMstackItem(stack, put_r, nullptr, nullptr);
    if (!ResultOk(put_r) || !put_it)
    {
        if (err)
            *err = "AMmapPutObject(" + std::string(key) + ") failed: " + ResultErrorStr(put_r);
        return nullptr;
    }
    if (const AMobjId* oid = AMitemObjId(put_it))
        return oid;
    if (err)
        *err = "AMmapPutObject(" + std::string(key) + "): missing obj id";
    return nullptr;
}

const AMobjId* GetOrCreateList(AMdoc* doc, AMstack** stack, const AMobjId* parent, std::string_view key, std::string* err)
{
    AMresult* get_r = AMmapGet(doc, parent, ByteSpan(key), nullptr);
    AMitem* get_it = AMstackItem(stack, get_r, nullptr, nullptr);
    if (!ResultOk(get_r) || !get_it)
    {
        if (err)
            *err = "AMmapGet(" + std::string(key) + ") failed: " + ResultErrorStr(get_r);
        return nullptr;
    }

    if (AMitemValType(get_it) != AM_VAL_TYPE_VOID)
    {
        if (const AMobjId* oid = AMitemObjId(get_it))
            return oid;
    }

    AMresult* put_r = AMmapPutObject(doc, parent, ByteSpan(key), AM_OBJ_TYPE_LIST);
    AMitem* put_it = AMstackItem(stack, put_r, nullptr, nullptr);
    if (!ResultOk(put_r) || !put_it)
    {
        if (err)
            *err = "AMmapPutObject(list " + std::string(key) + ") failed: " + ResultErrorStr(put_r);
        return nullptr;
    }
    if (const AMobjId* oid = AMitemObjId(put_it))
        return oid;
    if (err)
        *err = "AMmapPutObject(list " + std::string(key) + "): missing obj id";
    return nullptr;
}

const AMobjId* EnsureLayerMapAt(AMdoc* doc, AMstack** stack, const AMobjId* layers_list, std::size_t idx, std::string* err)
{
    // Ensure list length > idx by appending maps.
    while (AMobjSize(doc, layers_list, nullptr) <= idx)
    {
        const std::size_t pos = AMobjSize(doc, layers_list, nullptr);
        AMresult* r = AMlistPutObject(doc, layers_list, pos, true /*insert*/, AM_OBJ_TYPE_MAP);
        AMitem* it = AMstackItem(stack, r, nullptr, nullptr);
        if (!ResultOk(r) || !it)
        {
            if (err)
                *err = "AMlistPutObject(layer) failed: " + ResultErrorStr(r);
            return nullptr;
        }
    }

    AMresult* get_r = AMlistGet(doc, layers_list, idx, nullptr);
    AMitem* get_it = AMstackItem(stack, get_r, nullptr, nullptr);
    if (!ResultOk(get_r) || !get_it)
    {
        if (err)
            *err = "AMlistGet(layer) failed: " + ResultErrorStr(get_r);
        return nullptr;
    }
    if (const AMobjId* oid = AMitemObjId(get_it))
        return oid;
    if (err)
        *err = "AMlistGet(layer): missing obj id";
    return nullptr;
}

std::string TileKey(std::int64_t ty, std::int64_t tx)
{
    std::ostringstream oss;
    oss << ty << "," << tx;
    return oss.str();
}

} // namespace

bool EnsureCanvasTileSchemaV1(AutomergeAdapter& doc, const CanvasTileSchemaV1Config& cfg, std::string* err)
{
    if (!doc.Valid())
    {
        if (err)
            *err = "EnsureCanvasTileSchemaV1: doc not initialized";
        return false;
    }
    if (cfg.columns <= 0 || cfg.rows <= 0 || cfg.tile_size <= 0)
    {
        if (err)
            *err = "EnsureCanvasTileSchemaV1: invalid dimensions";
        return false;
    }

    AMstack* stack = nullptr;
    AMdoc* d = doc.Raw();

    // root.schema = 1
    {
        AMresult* r = AMmapPutInt(d, AM_ROOT, ByteSpan("schema"), cfg.schema);
        AMstackItem(&stack, r, nullptr, nullptr);
        if (!ResultOk(r))
        {
            if (err)
                *err = "AMmapPutInt(root.schema) failed: " + ResultErrorStr(r);
            AMstackFree(&stack);
            return false;
        }
    }

    // root.canvas (map)
    const AMobjId* canvas = GetOrCreateMap(d, &stack, AM_ROOT, "canvas", err);
    if (!canvas)
    {
        AMstackFree(&stack);
        return false;
    }

    auto put_int = [&](std::string_view k, std::int64_t v) -> bool {
        AMresult* r = AMmapPutInt(d, canvas, ByteSpan(k), v);
        AMstackItem(&stack, r, nullptr, nullptr);
        if (!ResultOk(r))
        {
            if (err)
                *err = "AMmapPutInt(canvas." + std::string(k) + ") failed: " + ResultErrorStr(r);
            return false;
        }
        return true;
    };

    if (!put_int("columns", cfg.columns) || !put_int("rows", cfg.rows) || !put_int("tile_size", cfg.tile_size) ||
        !put_int("tile_fmt", cfg.tile_fmt))
    {
        AMstackFree(&stack);
        return false;
    }

    // canvas.layers (list)
    const AMobjId* layers = GetOrCreateList(d, &stack, canvas, "layers", err);
    if (!layers)
    {
        AMstackFree(&stack);
        return false;
    }

    // Ensure layer 0 exists and has tiles map.
    const AMobjId* layer0 = EnsureLayerMapAt(d, &stack, layers, 0, err);
    if (!layer0)
    {
        AMstackFree(&stack);
        return false;
    }
    const AMobjId* tiles = GetOrCreateMap(d, &stack, layer0, "tiles", err);
    if (!tiles)
    {
        AMstackFree(&stack);
        return false;
    }
    (void)tiles;

    AMstackFree(&stack);
    return true;
}

bool PutTileV1(AutomergeAdapter& doc, std::size_t layer_index, std::int64_t ty, std::int64_t tx,
               std::span<const std::uint8_t> tile_bytes, std::string* err)
{
    if (!doc.Valid())
    {
        if (err)
            *err = "PutTileV1: doc not initialized";
        return false;
    }
    if (ty < 0 || tx < 0)
    {
        if (err)
            *err = "PutTileV1: negative ty/tx";
        return false;
    }
    if (tile_bytes.empty())
    {
        if (err)
            *err = "PutTileV1: tile_bytes empty";
        return false;
    }

    AMstack* stack = nullptr;
    AMdoc* d = doc.Raw();

    const AMobjId* canvas = GetOrCreateMap(d, &stack, AM_ROOT, "canvas", err);
    if (!canvas)
    {
        AMstackFree(&stack);
        return false;
    }
    const AMobjId* layers = GetOrCreateList(d, &stack, canvas, "layers", err);
    if (!layers)
    {
        AMstackFree(&stack);
        return false;
    }
    const AMobjId* layer = EnsureLayerMapAt(d, &stack, layers, layer_index, err);
    if (!layer)
    {
        AMstackFree(&stack);
        return false;
    }
    const AMobjId* tiles = GetOrCreateMap(d, &stack, layer, "tiles", err);
    if (!tiles)
    {
        AMstackFree(&stack);
        return false;
    }

    const std::string k = TileKey(ty, tx);
    AMresult* r = AMmapPutBytes(d, tiles, ByteSpan(k), ByteSpan(tile_bytes));
    AMstackItem(&stack, r, nullptr, nullptr);
    if (!ResultOk(r))
    {
        if (err)
            *err = "AMmapPutBytes(tiles[" + k + "]) failed: " + ResultErrorStr(r);
        AMstackFree(&stack);
        return false;
    }

    AMstackFree(&stack);
    return true;
}

std::optional<std::vector<std::uint8_t>> GetTileV1(const AutomergeAdapter& doc, std::size_t layer_index, std::int64_t ty,
                                                   std::int64_t tx, std::string* err)
{
    if (!doc.Valid())
    {
        if (err)
            *err = "GetTileV1: doc not initialized";
        return std::nullopt;
    }
    if (ty < 0 || tx < 0)
    {
        if (err)
            *err = "GetTileV1: negative ty/tx";
        return std::nullopt;
    }

    AMstack* stack = nullptr;
    const AMdoc* d = doc.Raw();

    auto get_obj_from_map = [&](const AMobjId* parent, std::string_view key) -> const AMobjId* {
        AMresult* get_r = AMmapGet(d, parent, ByteSpan(key), nullptr);
        AMitem* get_it = AMstackItem(&stack, get_r, nullptr, nullptr);
        if (!ResultOk(get_r) || !get_it)
            return nullptr;
        if (AMitemValType(get_it) == AM_VAL_TYPE_VOID)
            return nullptr;
        return AMitemObjId(get_it);
    };

    const AMobjId* canvas = get_obj_from_map(AM_ROOT, "canvas");
    if (!canvas)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }
    const AMobjId* layers = get_obj_from_map(canvas, "layers");
    if (!layers)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }

    const std::size_t n = AMobjSize(d, layers, nullptr);
    if (layer_index >= n)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }

    AMresult* layer_r = AMlistGet(d, layers, layer_index, nullptr);
    AMitem* layer_it = AMstackItem(&stack, layer_r, nullptr, nullptr);
    if (!ResultOk(layer_r) || !layer_it)
    {
        if (err)
            *err = "AMlistGet(layer) failed: " + ResultErrorStr(layer_r);
        AMstackFree(&stack);
        return std::nullopt;
    }
    const AMobjId* layer = AMitemObjId(layer_it);
    if (!layer)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }

    const AMobjId* tiles = get_obj_from_map(layer, "tiles");
    if (!tiles)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }

    const std::string k = TileKey(ty, tx);
    AMresult* get_r = AMmapGet(d, tiles, ByteSpan(k), nullptr);
    AMitem* get_it = AMstackItem(&stack, get_r, nullptr, nullptr);
    if (!ResultOk(get_r) || !get_it)
    {
        if (err)
            *err = "AMmapGet(tiles[" + k + "]) failed: " + ResultErrorStr(get_r);
        AMstackFree(&stack);
        return std::nullopt;
    }

    if (AMitemValType(get_it) == AM_VAL_TYPE_VOID)
    {
        AMstackFree(&stack);
        return std::nullopt;
    }
    if (AMitemValType(get_it) != AM_VAL_TYPE_BYTES)
    {
        if (err)
            *err = "GetTileV1: value is not bytes";
        AMstackFree(&stack);
        return std::nullopt;
    }

    AMbyteSpan bs{};
    if (!AMitemToBytes(get_it, &bs))
    {
        if (err)
            *err = "GetTileV1: AMitemToBytes failed";
        AMstackFree(&stack);
        return std::nullopt;
    }

    std::vector<std::uint8_t> out(bs.src, bs.src + bs.count);
    AMstackFree(&stack);
    return out;
}
} // namespace phos::p2p


