#include "test_harness.h"

#include "net/p2p/automerge_adapter.h"
#include "net/p2p/blake3_util.h"

PHOS_TEST(automerge_sync_two_peers_converge)
{
    using namespace phos::p2p;

    // Deterministic actor ids derived from bytes.
    const auto a_id = Blake3_32("actor-a");
    const auto b_id = Blake3_32("actor-b");

    AutomergeAdapter a;
    AutomergeAdapter b;
    std::string err;
    PHOS_REQUIRE(a.CreateWithActorId(a_id, &err));
    PHOS_REQUIRE(b.CreateWithActorId(b_id, &err));

    // Make a commit on A.
    PHOS_REQUIRE(a.RootPutInt("x", 1, &err));
    PHOS_REQUIRE(a.Commit("set x=1", &err));

    // Run sync handshake until quiescent or max rounds.
    for (int round = 0; round < 64; ++round)
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

    PHOS_REQUIRE(a.Equal(b));
}


