#include "TestHarness.h"

#include "AppSetup.h"
#include "ArchiveReaderCache.h"
#include "ImageCore/ImageLoader.h"
#include "IniStore.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace
{
    int g_failuresInCurrentTest = 0;
    int g_totalFailures = 0;

    std::vector<std::function<void()>>& Teardowns()
    {
        static std::vector<std::function<void()>> teardowns;
        return teardowns;
    }

    // The test process (and the child processes it starts, which inherit the
    // variable) use an INI in this directory, never the user's.
    constexpr wchar_t kTestIniDirVariable[] = L"FICTURE2_TESTS_INI_DIR";

    std::filesystem::path TestIniDirectory()
    {
        wchar_t inherited[MAX_PATH] {};
        if (GetEnvironmentVariableW(kTestIniDirVariable, inherited, MAX_PATH) > 0)
        {
            return inherited;
        }
        wchar_t temp[MAX_PATH] {};
        GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path dir =
            std::filesystem::path(temp) / (L"FICture2Tests_Ini_" + std::to_wstring(GetCurrentProcessId()));
        SetEnvironmentVariableW(kTestIniDirVariable, dir.c_str());
        return dir;
    }

    // The user's INI as FICture2App::GetIniFilePath() resolves it without the
    // test override.
    std::wstring UserIniPath()
    {
        wchar_t appData[MAX_PATH] {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData)))
        {
            return {};
        }
        return (std::filesystem::path(appData) / L"FICture2" / L"FICture2.ini").wstring();
    }

    // A file's bytes, or nothing when it does not exist.
    std::optional<std::string> ReadBytes(const std::wstring& path)
    {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // The user's INI and the backup an interrupted commit leaves next to it.
    struct UserIniState
    {
        std::optional<std::string> ini;
        std::optional<std::string> backup;

        static UserIniState Read(const std::wstring& path)
        {
            return { ReadBytes(path), ReadBytes(IniStore::BackupPath(path)) };
        }

        bool operator==(const UserIniState&) const = default;
    };
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
    // Before anything reads the INI path: tests (and children) use their own.
    const std::filesystem::path testIniDir = TestIniDirectory();
    {
        std::error_code ec;
        std::filesystem::create_directories(testIniDir, ec);
    }
    FICture2App::SetIniFilePathForTesting((testIniDir / L"FICture2.ini").wstring());

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

    // Fails the run if anything still reached the user's INI.
    const std::wstring userIni = UserIniPath();
    const UserIniState userIniBefore = UserIniState::Read(userIni);

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

    if (!(UserIniState::Read(userIni) == userIniBefore))
    {
        ++g_totalFailures;
        std::fprintf(stderr, "the user's INI (or its commit backup) changed during the test run: "
            "a test reached %%LOCALAPPDATA%%\\FICture2 instead of the test INI\n");
    }
    {
        std::error_code ec;
        std::filesystem::remove_all(testIniDir, ec);
    }

    std::printf("%d test(s) run, %d failed\n", run, failedTests);
    if (run == 0)
    {
        std::fprintf(stderr, "no tests match prefix '%s'\n", prefix);
        return 1;
    }
    return (g_totalFailures == 0) ? 0 : 1;
}
