// Minimal header-only test harness (no external dependencies).
//
// Goals:
// - Single binary, fast iteration, easy to run under Makefile.
// - Good enough assertions for unit/property tests in headless multiplayer work.
//
// Usage:
//   #include "test_harness.h"
//   PHOS_TEST(my_test_name) { PHOS_REQUIRE(1 + 1 == 2); }
//
// Command line:
//   --list            List tests
//   --filter <substr> Run tests whose name contains <substr>

#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phos::test
{
struct Case
{
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Registry()
{
    static std::vector<Case> g;
    return g;
}

struct Registrar
{
    Registrar(const char* name, void (*fn)()) { Registry().push_back(Case{name, fn}); }
};

[[noreturn]] inline void Fail(std::string_view expr, const char* file, int line, std::string_view msg = {})
{
    std::string out;
    out.reserve(256);
    out += file;
    out += ":";
    out += std::to_string(line);
    out += ": REQUIRE failed: ";
    out += expr;
    if (!msg.empty())
    {
        out += " (";
        out += msg;
        out += ")";
    }
    throw std::runtime_error(out);
}

inline int Run(int argc, char** argv)
{
    std::string filter;
    bool list = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a(argv[i]);
        if (a == "--list")
            list = true;
        else if (a == "--filter")
        {
            if (i + 1 >= argc)
                throw std::runtime_error("--filter requires an argument");
            filter = argv[++i];
        }
    }

    const auto& cases = Registry();
    if (list)
    {
        for (const auto& c : cases)
            std::cout << c.name << "\n";
        return 0;
    }

    int failed = 0;
    int ran = 0;
    for (const auto& c : cases)
    {
        if (!filter.empty() && std::string_view(c.name).find(filter) == std::string_view::npos)
            continue;
        ++ran;
        try
        {
            c.fn();
            std::cout << "[PASS] " << c.name << "\n";
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::cout << "[FAIL] " << c.name << "\n  " << e.what() << "\n";
        }
        catch (...)
        {
            ++failed;
            std::cout << "[FAIL] " << c.name << "\n  (unknown exception)\n";
        }
    }

    if (ran == 0)
    {
        std::cout << "No tests matched";
        if (!filter.empty())
            std::cout << " filter='" << filter << "'";
        std::cout << ".\n";
        return 2;
    }

    std::cout << "Ran " << ran << " test(s), " << failed << " failed.\n";
    return failed == 0 ? 0 : 1;
}
} // namespace phos::test

#define PHOS_TEST(name)                                                                                               \
    static void phos_test_##name();                                                                                   \
    static ::phos::test::Registrar phos_test_reg_##name(#name, &phos_test_##name);                                    \
    static void phos_test_##name()

#define PHOS_REQUIRE(expr)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            ::phos::test::Fail(#expr, __FILE__, __LINE__);                                                             \
    } while (0)

#define PHOS_REQUIRE_MSG(expr, msg)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            ::phos::test::Fail(#expr, __FILE__, __LINE__, (msg));                                                      \
    } while (0)


