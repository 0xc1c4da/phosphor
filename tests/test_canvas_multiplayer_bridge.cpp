#include "test_harness.h"

#include "core/canvas.h"
#include "core/glyph_id.h"
#include "net/p2p/automerge_adapter.h"
#include "net/p2p/blake3_util.h"
#include "net/p2p/canvas_multiplayer_bridge.h"
#include "net/p2p/canvas_tile_schema_v1.h"

PHOS_TEST(canvas_multiplayer_bridge_roundtrip_tile_v1)
{
    using namespace phos::p2p;

    // Two Automerge docs with deterministic actor ids.
    AutomergeAdapter a;
    AutomergeAdapter b;
    std::string err;
    PHOS_REQUIRE(a.CreateWithActorId(Blake3_32("bridge-a"), &err));
    PHOS_REQUIRE(b.CreateWithActorId(Blake3_32("bridge-b"), &err));

    // Two canvases.
    AnsiCanvas ca(32);
    AnsiCanvas cb(32);
    ca.SetRows(32);
    cb.SetRows(32);

    // Ensure minimal schema.
    const CanvasTileSchemaV1Config cfg{
        .schema = 1,
        .columns = 32,
        .rows = 32,
        .tile_size = 16,
        .tile_fmt = 1,
    };
    PHOS_REQUIRE(EnsureCanvasTileSchemaV1(a, cfg, &err));
    PHOS_REQUIRE(EnsureCanvasTileSchemaV1(b, cfg, &err));

    // Mutate canvas A in a couple of places within tile (0,0).
    const int layer = 0;
    const auto g1 = phos::glyph::MakeUnicodeScalar(U'X');
    const auto g2 = phos::glyph::MakeUnicodeScalar(U'@');
    PHOS_REQUIRE(ca.SetLayerGlyphIndicesPartial(layer, 5, 6, g1, (AnsiCanvas::ColourIndex16)12, (AnsiCanvas::ColourIndex16)34,
                                               (AnsiCanvas::Attrs)(AnsiCanvas::Attr_Bold)));
    PHOS_REQUIRE(ca.SetLayerGlyphIndicesPartial(layer, 7, 8, g2, (AnsiCanvas::ColourIndex16)56, (AnsiCanvas::ColourIndex16)78,
                                               (AnsiCanvas::Attrs)(AnsiCanvas::Attr_Underline)));

    // Write tile into Automerge doc A and commit.
    PHOS_REQUIRE(PutCanvasTileV1(a, ca, layer, 16, /*ty=*/0, /*tx=*/0, &err));
    PHOS_REQUIRE(a.Commit("put tile 0,0", &err));

    // Sync to B until quiescent.
    for (int round = 0; round < 128; ++round)
    {
        bool progressed = false;
        if (auto msg = a.GenerateSync("b", &err))
        {
            progressed = true;
            PHOS_REQUIRE(b.ReceiveSync("a", *msg, &err));
        }
        if (auto msg = b.GenerateSync("a", &err))
        {
            progressed = true;
            PHOS_REQUIRE(a.ReceiveSync("b", *msg, &err));
        }
        if (!progressed)
            break;
    }

    // Apply the tile from doc B into canvas B.
    PHOS_REQUIRE(ApplyDocTileToCanvasV1(cb, b, layer, 16, /*ty=*/0, /*tx=*/0, &err));

    // Verify the two touched cells match.
    PHOS_REQUIRE(cb.GetLayerGlyph(layer, 5, 6) == g1);
    PHOS_REQUIRE(cb.GetLayerGlyph(layer, 7, 8) == g2);

    {
        AnsiCanvas::ColourIndex16 fg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::ColourIndex16 bg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::Attrs attrs = 0;
        PHOS_REQUIRE(cb.GetLayerCellIndices(layer, 5, 6, fg, bg));
        PHOS_REQUIRE(cb.GetLayerCellAttrs(layer, 5, 6, attrs));
        PHOS_REQUIRE(fg == (AnsiCanvas::ColourIndex16)12);
        PHOS_REQUIRE(bg == (AnsiCanvas::ColourIndex16)34);
        PHOS_REQUIRE(attrs == (AnsiCanvas::Attrs)AnsiCanvas::Attr_Bold);
    }

    {
        AnsiCanvas::ColourIndex16 fg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::ColourIndex16 bg = AnsiCanvas::kUnsetIndex16;
        AnsiCanvas::Attrs attrs = 0;
        PHOS_REQUIRE(cb.GetLayerCellIndices(layer, 7, 8, fg, bg));
        PHOS_REQUIRE(cb.GetLayerCellAttrs(layer, 7, 8, attrs));
        PHOS_REQUIRE(fg == (AnsiCanvas::ColourIndex16)56);
        PHOS_REQUIRE(bg == (AnsiCanvas::ColourIndex16)78);
        PHOS_REQUIRE(attrs == (AnsiCanvas::Attrs)AnsiCanvas::Attr_Underline);
    }
}


