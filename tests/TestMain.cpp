#include "TestHarness.h"

#include "ArchiveReaderCache.h"
#include "ImageCore/ImageLoader.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

namespace
{
    int g_failuresInCurrentTest = 0;
    int g_totalFailures = 0;

    std::vector<std::function<void()>>& Teardowns()
    {
        static std::vector<std::function<void()>> teardowns;
        return teardowns;
    }
}

namespace TestHarness
{
    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> registry;
        return registry;
    }

    void ReportFailure(const char* file, int line, const char* expression)
    {
        ++g_failuresInCurrentTest;
        ++g_totalFailures;
        std::fprintf(stderr, "  %s(%d): check failed: %s\n", file, line, expression);
    }

    void RegisterTeardown(std::function<void()> teardown)
    {
        Teardowns().push_back(std::move(teardown));
    }

    std::vector<ChildCase>& ChildRegistry()
    {
        static std::vector<ChildCase> registry;
        return registry;
    }
}

// Usage: FICture2Tests [--list | <name prefix> | --child <name> [args...]]
int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--child") == 0)
    {
        for (const auto& child : TestHarness::ChildRegistry())
        {
            if (std::strcmp(child.name, argv[2]) == 0)
            {
                return child.fn(argc - 3, argv + 3);
            }
        }
        std::fprintf(stderr, "unknown child: %s\n", argv[2]);
        return 2;
    }

    const char* prefix = (argc > 1) ? argv[1] : "";
    if (std::strcmp(prefix, "--list") == 0)
    {
        for (const auto& test : TestHarness::Registry())
        {
            std::printf("%s\n", test.name);
        }
        return 0;
    }

    int run = 0;
    int failedTests = 0;
    for (const auto& test : TestHarness::Registry())
    {
        if (std::strncmp(test.name, prefix, std::strlen(prefix)) != 0)
        {
            continue;
        }

        ++run;
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        try
        {
            test.fn();
        }
        catch (const TestHarness::RequireFailed&)
        {
        }
        catch (const std::exception& ex)
        {
            TestHarness::ReportFailure(__FILE__, __LINE__, ex.what());
        }
        catch (...)
        {
            TestHarness::ReportFailure(__FILE__, __LINE__, "unknown exception");
        }

        if (g_failuresInCurrentTest == 0)
        {
            std::printf("[  OK  ] %s\n", test.name);
        }
        else
        {
            ++failedTests;
            std::printf("[ FAIL ] %s\n", test.name);
        }
    }

    for (const auto& teardown : Teardowns())
    {
        teardown();
    }

    // Join the process-wide worker pools before static teardown (their
    // destructors would join without a bound). Neither call creates anything
    // that was never used.
    if (!ImageCore::ImageLoader::ShutdownIfCreated(std::chrono::seconds(10)) ||
        !Floar::ArchiveReaderCache::Shared().Shutdown(std::chrono::seconds(10)))
    {
        std::fprintf(stderr, "worker shutdown timed out; ending without static teardown\n");
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 3);
    }

    std::printf("%d test(s) run, %d failed\n", run, failedTests);
    if (run == 0)
    {
        std::fprintf(stderr, "no tests match prefix '%s'\n", prefix);
        return 1;
    }
    return (g_totalFailures == 0) ? 0 : 1;
}
