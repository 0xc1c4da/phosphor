#include "test_harness.h"

#include <string>

PHOS_TEST(smoke_basic)
{
    PHOS_REQUIRE(1 + 1 == 2);
    PHOS_REQUIRE(std::string("phosphor").size() == 8);
}


