#pragma once

// Minimal first-party test harness (no external framework).
//
//   FIC2_TEST(Suite_Name) { FIC2_CHECK(x == 1); FIC2_REQUIRE(ptr != nullptr); }
//
// FIC2_CHECK records a failure and continues; FIC2_REQUIRE records a failure and
// aborts the current test. The runner (TestMain.cpp) executes every registered
// test whose name starts with the optional command-line prefix and returns a
// non-zero exit code when any check failed.

#include <functional>
#include <string>
#include <vector>

namespace TestHarness
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* name { nullptr };
        TestFn fn { nullptr };
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(const char* name, TestFn fn)
        {
            Registry().push_back({ name, fn });
        }
    };

    struct RequireFailed
    {
    };

    void ReportFailure(const char* file, int line, const char* expression);

    // Runs once after all selected tests (e.g. to shut down process singletons).
    void RegisterTeardown(std::function<void()> teardown);

    // Child-process entry points, for paths that end their own process:
    // `FICture2Tests --child <name> [args...]` runs one and exits with its result.
    using ChildFn = int (*)(int argc, char** argv);  // the args after <name>

    struct ChildCase
    {
        const char* name { nullptr };
        ChildFn fn { nullptr };
    };

    std::vector<ChildCase>& ChildRegistry();

    struct ChildRegistrar
    {
        ChildRegistrar(const char* name, ChildFn fn)
        {
            ChildRegistry().push_back({ name, fn });
        }
    };
}

#define FIC2_TEST(name) \
    static void name(); \
    static const TestHarness::Registrar name##_registrar(#name, &name); \
    static void name()

#define FIC2_CHILD(name) \
    static int name(int argc, char** argv); \
    static const TestHarness::ChildRegistrar name##_registrar(#name, &name); \
    static int name(int argc, char** argv)

#define FIC2_CHECK(expr) \
    do { \
        if (!(expr)) \
        { \
            TestHarness::ReportFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define FIC2_REQUIRE(expr) \
    do { \
        if (!(expr)) \
        { \
            TestHarness::ReportFailure(__FILE__, __LINE__, #expr); \
            throw TestHarness::RequireFailed {}; \
        } \
    } while (0)
