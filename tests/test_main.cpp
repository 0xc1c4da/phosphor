#include "test_harness.h"

int main(int argc, char** argv)
{
    try
    {
        return phos::test::Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        // Argument parsing / harness-level failure.
        std::cout << "[HARNESS FAIL]\n  " << e.what() << "\n";
        return 2;
    }
}


