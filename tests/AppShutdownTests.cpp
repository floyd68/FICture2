#include "TestHarness.h"

#include "AppIpc.h"
#include "AppLog.h"
#include "AppShutdown.h"
#include "ArchiveCancelTestSupport.h"
#include "CommonUtil.h"
#include "FolderEnumeration.h"
#include "ImageAsyncBinding.h"

#include "ImageCore/IPathByteSource.h"
#include "ImageCore/ImageLoader.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <ole2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The post-UI cleanup (RunAppShutdown): stage order on the normal and startup
// failure paths, repeated cleanup, the bounded-join / forced-exit decision per
// stage, and - in child processes, against the real ImageCore and an injected
// Floar archive cache - services that never started, a cooperative shutdown,
// and the forced exits of a stuck Floar open and a stuck ImageCore read.

namespace
{
    using namespace ArchiveCancelTest;

    // Hooks that record one event per stage; bounded stages report `joined`.
    struct Recorder
    {
        std::vector<std::string> events {};
        std::vector<std::chrono::milliseconds> bounds {};
        AppShutdownStage timedOut { AppShutdownStage::IpcServer };
        bool joined[4] { true, true, true, true };   // IPC, enumeration, ImageCore, Floar

        struct Forced
        {
            AppShutdownStage stage {};
            std::chrono::milliseconds bound {};
            UINT exitCode { 0 };
        };
        std::vector<Forced> forced {};

        AppShutdownHooks Hooks()
        {
            AppShutdownHooks hooks {};
            hooks.stopIpcServer = [this](std::chrono::milliseconds bound)
            {
                events.push_back("ipc");
                bounds.push_back(bound);
                return joined[0];
            };
            hooks.finishFolderEnumeration = [this](std::chrono::milliseconds bound)
            {
                events.push_back("enumeration");
                bounds.push_back(bound);
                return joined[1];
            };
            hooks.shutdownImageCore = [this](std::chrono::milliseconds bound)
            {
                events.push_back("imagecore");
                bounds.push_back(bound);
                return joined[2];
            };
            hooks.shutdownArchiveOpens = [this](std::chrono::milliseconds bound)
            {
                events.push_back("floar");
                bounds.push_back(bound);
                return joined[3];
            };
            hooks.clearGpuCache = [this] { events.push_back("gpu"); };
            hooks.shutdownFd2d = [this] { events.push_back("fd2d"); };
            hooks.shutdownLog = [this] { events.push_back("log"); };
            hooks.uninitializeOle = [this] { events.push_back("ole"); };
            hooks.forcedExit = [this](AppShutdownStage stage, std::chrono::milliseconds bound, UINT exitCode)
            {
                events.push_back("forced");
                forced.push_back({ stage, bound, exitCode });
            };
            return hooks;
        }
    };

    const std::vector<std::string> kFullOrder {
        "ipc", "enumeration", "imagecore", "floar", "gpu", "fd2d", "log", "ole" };

    const AppExitPath kAllPaths[] {
        AppExitPath::Normal,
        AppExitPath::OleInitFailed,
        AppExitPath::Fd2dInitFailed,
        AppExitPath::WindowCreationFailed,
    };

    // ---- child-process plumbing -------------------------------------------

    // The last command-line argument, read wide (it may be outside the ANSI code page).
    std::filesystem::path ChildDirArgument()
    {
        int wideArgc = 0;
        LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        std::filesystem::path dir;
        if (wideArgv != nullptr && wideArgc >= 4)
        {
            dir = wideArgv[wideArgc - 1];
        }
        if (wideArgv != nullptr)
        {
            LocalFree(wideArgv);
        }
        return dir;
    }

    void Touch(const std::filesystem::path& file)
    {
        std::ofstream(file) << "1";
    }

    // Writes `marker` when static destruction runs (it must not on a forced exit).
    struct StaticTeardownSentinel
    {
        std::filesystem::path marker;
        ~StaticTeardownSentinel()
        {
            if (!marker.empty())
            {
                Touch(marker);
            }
        }
    };

    struct ChildRun
    {
        bool finished { false };
        DWORD exitCode { 0 };
        DWORD processId { 0 };
        std::chrono::milliseconds elapsed {};
    };

    ChildRun RunChild(const char* name, const std::filesystem::path& dir)
    {
        ChildRun run {};
        wchar_t exe[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        {
            return run;
        }
        std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --child " +
            std::wstring(name, name + std::strlen(name)) + L" \"" + dir.wstring() + L"\"";

        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        const auto start = std::chrono::steady_clock::now();
        if (!CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            return run;
        }
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        run.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        run.finished = (wait == WAIT_OBJECT_0);
        if (!run.finished)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        GetExitCodeProcess(process.hProcess, &run.exitCode);
        run.processId = process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return run;
    }

    std::string ReadLog(const std::filesystem::path& dir, DWORD processId)
    {
        std::ifstream logFile(dir / (L"FICture2_" + std::to_wstring(processId) + L".log"));
        return std::string((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    }

    // Child-side check: prints the failed condition and counts it.
    struct ChildChecks
    {
        int failures { 0 };
        void Check(bool condition, const char* what)
        {
            if (!condition)
            {
                ++failures;
                std::fprintf(stderr, "  child check failed: %s\n", what);
            }
        }
    };
#define CHILD_CHECK(checks, expr) (checks).Check((expr), #expr)

    // Hooks after the bounded stages that leave a marker file instead of
    // touching the real GPU cache, FD2D, log or OLE.
    void UseMarkerTeardown(AppShutdownHooks& hooks, const std::filesystem::path& dir)
    {
        hooks.clearGpuCache = [dir] { Touch(dir / L"gpu.txt"); };
        hooks.shutdownFd2d = [dir] { Touch(dir / L"fd2d.txt"); };
        hooks.shutdownLog = [dir] { Touch(dir / L"log.txt"); };
        hooks.uninitializeOle = [dir] { Touch(dir / L"ole.txt"); };
    }

    ImageCore::ImageRequest Thumb(const std::wstring& path)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::Thumbnail);
        request.targetSize = ImageCore::Size { 64.0f, 64.0f };
        request.allowGpuCompressedDDS = false;
        return request;
    }

    // A legacy (ReadAll-only) source whose read ignores its token and OS
    // cancellation: it waits on an event that is never set.
    class StuckLegacySource final : public ImageCore::IPathByteSource
    {
    public:
        HANDLE entered { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        HANDLE never { CreateEventW(nullptr, TRUE, FALSE, nullptr) };

        std::wstring ResolveHostPath(const std::wstring& path) const override { return path; }
        std::vector<uint8_t> ReadAll(const std::wstring&) const override
        {
            SetEvent(entered);
            WaitForSingleObject(never, INFINITE);
            return {};
        }
    };
}

// -------------------------------------------------------------------------
// Order and decisions (recording hooks, in process)
// -------------------------------------------------------------------------

FIC2_TEST(AppShutdown_SameStageOrderOnNormalAndFailurePaths)
{
    for (const AppExitPath path : kAllPaths)
    {
        Recorder recorder;
        FIC2_CHECK(RunAppShutdown(path, recorder.Hooks()) == AppShutdownResult::Completed);
        FIC2_CHECK(recorder.events == kFullOrder);
        FIC2_CHECK(recorder.forced.empty());
        // The declared bounds reach the bounded stages.
        const std::vector<std::chrono::milliseconds> declared {
            kIpcServerShutdownBound, kFolderEnumerationShutdownBound, kImageCoreShutdownBound, kArchiveOpenShutdownBound };
        FIC2_CHECK(recorder.bounds == declared);
    }
}

FIC2_TEST(AppShutdown_RepeatedCleanupRunsTheSamePolicy)
{
    Recorder recorder;
    const AppShutdownHooks hooks = recorder.Hooks();
    FIC2_CHECK(RunAppShutdown(AppExitPath::WindowCreationFailed, hooks) == AppShutdownResult::Completed);
    FIC2_CHECK(RunAppShutdown(AppExitPath::Normal, hooks) == AppShutdownResult::Completed);
    std::vector<std::string> twice = kFullOrder;
    twice.insert(twice.end(), kFullOrder.begin(), kFullOrder.end());
    FIC2_CHECK(recorder.events == twice);
}

FIC2_TEST(AppShutdown_TimedOutStageTakesForcedExitBeforeAnyLaterStage)
{
    struct Case
    {
        int stageIndex;
        AppShutdownStage stage;
        UINT exitCode;
        std::vector<std::string> events;
    };
    const Case cases[] {
        { 0, AppShutdownStage::IpcServer, kIpcServerForcedExitCode, { "ipc", "forced" } },
        { 1, AppShutdownStage::FolderEnumeration, kFolderEnumerationForcedExitCode, { "ipc", "enumeration", "forced" } },
        { 2, AppShutdownStage::ImageCore, kImageCoreForcedExitCode, { "ipc", "enumeration", "imagecore", "forced" } },
        { 3, AppShutdownStage::ArchiveOpens, kArchiveOpenForcedExitCode, { "ipc", "enumeration", "imagecore", "floar", "forced" } },
    };
    AppShutdownBounds bounds {};
    bounds.ipcServer = std::chrono::milliseconds(7);
    bounds.folderEnumeration = std::chrono::milliseconds(11);
    bounds.imageCore = std::chrono::milliseconds(22);
    bounds.archiveOpens = std::chrono::milliseconds(33);
    const std::chrono::milliseconds expectedBound[] {
        bounds.ipcServer, bounds.folderEnumeration, bounds.imageCore, bounds.archiveOpens };

    for (const Case& c : cases)
    {
        Recorder recorder;
        recorder.joined[c.stageIndex] = false;
        FIC2_CHECK(RunAppShutdown(AppExitPath::Normal, recorder.Hooks(), bounds) == AppShutdownResult::ForcedExit);
        FIC2_CHECK(recorder.events == c.events);
        FIC2_REQUIRE(recorder.forced.size() == 1);
        FIC2_CHECK(recorder.forced[0].stage == c.stage);
        FIC2_CHECK(recorder.forced[0].exitCode == c.exitCode);
        FIC2_CHECK(recorder.forced[0].bound == expectedBound[c.stageIndex]);
    }

    // The exit codes are distinct, so the exit code names the stage.
    const std::set<UINT> codes {
        kIpcServerForcedExitCode, kFolderEnumerationForcedExitCode, kImageCoreForcedExitCode, kArchiveOpenForcedExitCode };
    FIC2_CHECK(codes.size() == 4);
}

FIC2_TEST(AppShutdown_DefaultOleStageUninitializesAtMostOnce)
{
    const HRESULT first = OleInitialize(nullptr);
    if (first != S_OK)
    {
        // COM was already initialized on this thread: the count is not observable.
        if (SUCCEEDED(first))
        {
            OleUninitialize();
        }
        return;
    }
    // Two references; the hook may release only the one it owns.
    FIC2_REQUIRE(OleInitialize(nullptr) == S_FALSE);
    const AppShutdownHooks hooks = DefaultAppShutdownHooks(true);
    hooks.uninitializeOle();
    hooks.uninitializeOle();
    const HRESULT after = OleInitialize(nullptr);
    FIC2_CHECK(after == S_FALSE);   // still initialized: released once, not twice
    if (SUCCEEDED(after))
    {
        OleUninitialize();
    }
    OleUninitialize();

    // Without OLE ownership the hook never uninitializes.
    FIC2_REQUIRE(OleInitialize(nullptr) == S_OK);
    DefaultAppShutdownHooks(false).uninitializeOle();
    const HRESULT unowned = OleInitialize(nullptr);
    FIC2_CHECK(unowned == S_FALSE);
    if (SUCCEEDED(unowned))
    {
        OleUninitialize();
    }
    OleUninitialize();
}

// -------------------------------------------------------------------------
// (a) + (b): services never started, cleanup called twice (real stages)
// -------------------------------------------------------------------------

FIC2_CHILD(AppShutdownNeverStartedChild)
{
    (void)argc;
    (void)argv;
    ChildChecks checks;
    CHILD_CHECK(checks, !ImageCore::ImageLoader::IsInstantiated());

    // Every real stage (OLE not owned, as after an OleInitialize failure).
    const AppShutdownHooks hooks = DefaultAppShutdownHooks(false);
    for (const AppExitPath path : { AppExitPath::OleInitFailed, AppExitPath::Normal })
    {
        CHILD_CHECK(checks, RunAppShutdown(path, hooks) == AppShutdownResult::Completed);
        // No loader was created and no ImageCore, Floar or enumeration worker started.
        CHILD_CHECK(checks, !ImageCore::ImageLoader::IsInstantiated());
        CHILD_CHECK(checks, Floar::ArchiveReaderCache::Shared().WorkerCount() == 0);
        CHILD_CHECK(checks, Floar::ArchiveReaderCache::Shared().OpenCount() == 0);
        CHILD_CHECK(checks, FolderEnumerationService::Shared().WorkerCount() == 0);
    }
    return checks.failures == 0 ? 0 : 1;
}

FIC2_TEST(AppShutdown_ServicesNeverStartedAndRepeatedCleanup)
{
    const std::filesystem::path dir = MakeTempDir(L"neverstarted");
    const ChildRun run = RunChild("AppShutdownNeverStartedChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -------------------------------------------------------------------------
// (c): ImageCore releases and joins its caller first, then the Floar open in
// flight joins during the Floar stage, then GPU/FD2D teardown runs
// -------------------------------------------------------------------------

FIC2_CHILD(AppShutdownCooperativeChild)
{
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = ChildDirArgument();
    if (dir.empty())
    {
        return 3;
    }
    ChildChecks checks;
    (void)CountingDecoderFactory::Instance();

    // A cooperative open: it waits on its stop token; once stopped it returns
    // only after the Floar stage began, so the open is still in flight then.
    GateArchive archive;
    archive.AddEntry(L"a.fic2c", false);
    GateOpener opener(archive);
    opener.blockOpen = true;
    std::atomic<bool> floarStageEntered { false };
    std::atomic<bool> openerReturned { false };
    opener.afterStop = [&]
    {
        (void)WaitUntil([&] { return floarStageEntered.load(); });
        openerReturned.store(true);
    };
    auto* cache = new Floar::ArchiveReaderCache(Floar::ArchiveReaderCache::kDefaultCapacity, opener.Make());
    auto* source = new FloarPathByteSource(*cache);
    ImageCore::SetPathByteSource(source);

    const std::wstring path = CommonUtil::NormalizePath((dir / L"A.zip").wstring() + L"\\a.fic2c");
    ImageAsyncBinding binding;
    binding.RequestLoad(Thumb(path), path);
    if (!opener.WaitEntered(1) || !WaitUntil([&] { return cache->WaitingCount() == 1; }))
    {
        return 4;
    }

    std::vector<std::string> events;
    AppShutdownHooks defaults = DefaultAppShutdownHooks(false, {}, *cache);
    AppShutdownHooks hooks {};
    hooks.stopIpcServer = [&](std::chrono::milliseconds) { events.push_back("ipc"); return true; };
    hooks.finishFolderEnumeration = [&](std::chrono::milliseconds bound)
    {
        events.push_back("enumeration");
        return defaults.finishFolderEnumeration(bound);
    };
    hooks.shutdownImageCore = [&](std::chrono::milliseconds bound)
    {
        events.push_back("imagecore");
        const bool joined = defaults.shutdownImageCore(bound);
        CHILD_CHECK(checks, joined);
        // The request token released ImageCore's thread from the open wait.
        CHILD_CHECK(checks, cache->WaitingCount() == 0);
        CHILD_CHECK(checks, ImageCore::ImageLoader::Instance().LiveRequestCount() == 0);
        return joined;
    };
    hooks.shutdownArchiveOpens = [&](std::chrono::milliseconds bound)
    {
        events.push_back("floar");
        // The open is still running on a live worker when this stage begins.
        CHILD_CHECK(checks, cache->RunningOpenCount() == 1);
        CHILD_CHECK(checks, cache->WorkerCount() == 1);
        CHILD_CHECK(checks, !openerReturned.load());
        floarStageEntered.store(true);
        const bool joined = defaults.shutdownArchiveOpens(bound);
        CHILD_CHECK(checks, joined);
        CHILD_CHECK(checks, openerReturned.load());
        CHILD_CHECK(checks, cache->WorkerCount() == 0);
        CHILD_CHECK(checks, cache->RunningOpenCount() == 0);
        return joined;
    };
    hooks.clearGpuCache = [&] { events.push_back("gpu"); };
    hooks.shutdownFd2d = [&] { events.push_back("fd2d"); };
    hooks.shutdownLog = [&] { events.push_back("log"); };
    hooks.uninitializeOle = [&] { events.push_back("ole"); };
    hooks.forcedExit = [&](AppShutdownStage, std::chrono::milliseconds, UINT) { events.push_back("forced"); };

    CHILD_CHECK(checks, RunAppShutdown(AppExitPath::Normal, hooks) == AppShutdownResult::Completed);
    CHILD_CHECK(checks, events == kFullOrder);
    // The canceled request neither decoded nor completed.
    CHILD_CHECK(checks, CountingDecoderFactory::Instance().TotalDecodes() == 0);
    CHILD_CHECK(checks, binding.CurrentPhase() == ImageAsyncBinding::Phase::Superseded);
    CHILD_CHECK(checks, archive.Extractions().empty());
    CHILD_CHECK(checks, opener.StopSeen() == 1);

    ImageCore::SetPathByteSource(nullptr);
    delete source;
    delete cache;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return checks.failures == 0 ? 0 : 1;
}

FIC2_TEST(AppShutdown_CooperativeShutdownJoinsImageCoreThenFloarThenTearsDown)
{
    const std::filesystem::path dir = MakeTempDir(L"cooperative");
    const ChildRun run = RunChild("AppShutdownCooperativeChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -------------------------------------------------------------------------
// (d): a non-cooperative Floar open takes the forced exit at the Floar stage
// -------------------------------------------------------------------------

namespace
{
    constexpr std::chrono::milliseconds kChildBound { 300 };
}

FIC2_CHILD(AppShutdownStuckArchiveOpenChild)
{
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = ChildDirArgument();
    if (dir.empty())
    {
        return 3;
    }
    static StaticTeardownSentinel sentinel { dir / L"static-teardown.txt" };
    AppLog::Init(dir.wstring());
    (void)CountingDecoderFactory::Instance();

    // Never destroyed: the stuck worker keeps using them until the process ends.
    auto* archive = new GateArchive();
    archive->AddEntry(L"a.fic2c", false);
    auto* opener = new GateOpener(*archive);
    opener->blockOpen = true;
    opener->honourStop = false;   // ignores its token (and is not in an OS call)
    auto* cache = new Floar::ArchiveReaderCache(Floar::ArchiveReaderCache::kDefaultCapacity, opener->Make());
    auto* source = new FloarPathByteSource(*cache);
    ImageCore::SetPathByteSource(source);

    const std::wstring path = CommonUtil::NormalizePath((dir / L"A.zip").wstring() + L"\\a.fic2c");
    auto* binding = new ImageAsyncBinding();
    binding->RequestLoad(Thumb(path), path);
    if (!opener->WaitEntered(1) || !WaitUntil([&] { return cache->WaitingCount() == 1; }))
    {
        return 4;
    }

    AppShutdownHooks hooks = DefaultAppShutdownHooks(false, [dir] { Touch(dir / L"before-forced.txt"); }, *cache);
    const auto realImageCore = hooks.shutdownImageCore;
    hooks.shutdownImageCore = [realImageCore, dir](std::chrono::milliseconds bound)
    {
        const bool joined = realImageCore(bound);
        if (joined)
        {
            Touch(dir / L"imagecore-joined.txt");
        }
        return joined;
    };
    const auto realFloar = hooks.shutdownArchiveOpens;
    hooks.shutdownArchiveOpens = [realFloar, dir](std::chrono::milliseconds bound)
    {
        Touch(dir / L"floar-begin.txt");
        return realFloar(bound);
    };
    UseMarkerTeardown(hooks, dir);
    AppShutdownBounds bounds {};
    bounds.archiveOpens = kChildBound;
    RunAppShutdown(AppExitPath::Normal, hooks, bounds);
    return 5;  // not reached: the forced path ends the process
}

FIC2_TEST(AppShutdownForcedExit_StuckArchiveOpenEndsProcessAtFloarStage)
{
    const std::filesystem::path dir = MakeTempDir(L"stuckopen");
    const ChildRun run = RunChild("AppShutdownStuckArchiveOpenChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == kArchiveOpenForcedExitCode);
    FIC2_CHECK(run.elapsed >= kChildBound);

    // ImageCore was released and joined first; the Floar stage timed out.
    FIC2_CHECK(std::filesystem::exists(dir / L"imagecore-joined.txt"));
    FIC2_CHECK(std::filesystem::exists(dir / L"floar-begin.txt"));
    FIC2_CHECK(std::filesystem::exists(dir / L"before-forced.txt"));
    // No later stage and no static destructor ran beside the stuck worker.
    FIC2_CHECK(!std::filesystem::exists(dir / L"gpu.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"fd2d.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"log.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"ole.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"static-teardown.txt"));

    const std::string log = ReadLog(dir, run.processId);
    FIC2_CHECK(log.find("Floar archive opens still running 300 ms after shutdown") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -------------------------------------------------------------------------
// (e): a non-cooperative ImageCore read takes the forced exit at the ImageCore
// stage, before the Floar stage runs
// -------------------------------------------------------------------------

FIC2_CHILD(AppShutdownStuckImageCoreReadChild)
{
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = ChildDirArgument();
    if (dir.empty())
    {
        return 3;
    }
    static StaticTeardownSentinel sentinel { dir / L"static-teardown.txt" };
    AppLog::Init(dir.wstring());
    (void)CountingDecoderFactory::Instance();

    auto* source = new StuckLegacySource();
    ImageCore::SetPathByteSource(source);
    const std::wstring path = CommonUtil::NormalizePath((dir / L"stuck.fic2c").wstring());
    auto* binding = new ImageAsyncBinding();
    binding->RequestLoad(Thumb(path), path);
    if (WaitForSingleObject(source->entered, 10000) != WAIT_OBJECT_0)
    {
        return 4;
    }

    AppShutdownHooks hooks = DefaultAppShutdownHooks(false, [dir] { Touch(dir / L"before-forced.txt"); });
    hooks.shutdownArchiveOpens = [dir](std::chrono::milliseconds)
    {
        Touch(dir / L"floar.txt");
        return true;
    };
    UseMarkerTeardown(hooks, dir);
    AppShutdownBounds bounds {};
    bounds.imageCore = kChildBound;
    RunAppShutdown(AppExitPath::Normal, hooks, bounds);
    return 5;  // not reached: the forced path ends the process
}

FIC2_TEST(AppShutdownForcedExit_StuckImageCoreReadEndsProcessAtImageCoreStage)
{
    const std::filesystem::path dir = MakeTempDir(L"stuckread");
    const ChildRun run = RunChild("AppShutdownStuckImageCoreReadChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == kImageCoreForcedExitCode);
    FIC2_CHECK(run.elapsed >= kChildBound);

    FIC2_CHECK(std::filesystem::exists(dir / L"before-forced.txt"));
    // The Floar stage, later stages and static destructors never ran.
    FIC2_CHECK(!std::filesystem::exists(dir / L"floar.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"gpu.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"fd2d.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"log.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"ole.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"static-teardown.txt"));

    const std::string log = ReadLog(dir, run.processId);
    FIC2_CHECK(log.find("ImageCore still running 300 ms after shutdown") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -------------------------------------------------------------------------
// (f) + (g): the IPC server stage. With an idle accept wait and a connected
// client that sends nothing, the real stage joins every server thread before
// the log and static teardown; a request handler that never returns takes
// the forced exit at the IPC stage, before any later stage.
// -------------------------------------------------------------------------

namespace
{
    std::wstring ChildPipeName()
    {
        return L"\\\\.\\pipe\\FICture2_IPC_test_" + std::to_wstring(GetCurrentProcessId()) + L"_child";
    }

    std::set<DWORD> ProcessThreadIds()
    {
        std::set<DWORD> ids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return ids;
        }
        THREADENTRY32 entry { sizeof(entry) };
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
            {
                ids.insert(entry.th32ThreadID);
            }
        }
        CloseHandle(snapshot);
        return ids;
    }

    bool ThreadAlive(DWORD id)
    {
        HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, id);
        if (thread == nullptr)
        {
            return false;   // gone
        }
        const bool alive = WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
        CloseHandle(thread);
        return alive;
    }

    // Connects without sending anything; the handle stays open.
    HANDLE ConnectSilentClient(const std::wstring& name)
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        (void)WaitUntil([&]
        {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            return pipe != INVALID_HANDLE_VALUE;
        });
        return pipe;
    }
}

FIC2_CHILD(AppShutdownIpcCleanExitChild)
{
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = ChildDirArgument();
    if (dir.empty())
    {
        return 3;
    }
    static StaticTeardownSentinel sentinel { dir / L"static-teardown.txt" };
    AppLog::Init(dir.wstring());
    ChildChecks checks;

    const std::wstring name = ChildPipeName();
    const std::set<DWORD> before = ProcessThreadIds();
    AppIpc::Server server;
    if (!server.Start(name, [](const std::wstring&) { return AppIpc::Decision::CompareStarted; }))
    {
        return 4;
    }
    // A served request, then a client that connects and sends nothing; the
    // accept thread is back in its wait for the next client.
    AppIpc::Decision decision = AppIpc::Decision::Ignore;
    CHILD_CHECK(checks, AppIpc::TrySendPath(name, L"C:\\a.png", decision));
    CHILD_CHECK(checks, decision == AppIpc::Decision::CompareStarted);
    HANDLE silent = ConnectSilentClient(name);
    if (silent == INVALID_HANDLE_VALUE ||
        !WaitUntil([&] { return server.ClientWorkerCount() == 1 && WaitNamedPipeW(name.c_str(), 0) != FALSE; }))
    {
        return 4;
    }
    CHILD_CHECK(checks, server.WorkerCount() == 2);
    std::set<DWORD> serverThreads;
    for (const DWORD id : ProcessThreadIds())
    {
        if (before.count(id) == 0)
        {
            serverThreads.insert(id);
        }
    }
    CHILD_CHECK(checks, serverThreads.size() >= 2);

    AppShutdownHooks hooks = DefaultAppShutdownHooks(false, {}, Floar::ArchiveReaderCache::Shared(), &server);
    const auto realIpc = hooks.stopIpcServer;
    hooks.stopIpcServer = [&](std::chrono::milliseconds bound)
    {
        const auto start = std::chrono::steady_clock::now();
        const bool joined = realIpc(bound);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHILD_CHECK(checks, joined);
        // The stop event woke the accept wait and the silent client's read.
        CHILD_CHECK(checks, elapsed < std::chrono::milliseconds(500));
        CHILD_CHECK(checks, server.WorkerCount() == 0);
        // No server thread is left when the later stages, the log shutdown
        // and the static teardown run.
        for (const DWORD id : serverThreads)
        {
            CHILD_CHECK(checks, !ThreadAlive(id));
        }
        return joined;
    };
    hooks.shutdownLog = [&]
    {
        Touch(dir / L"log.txt");
        AppLog::Shutdown();
    };
    CHILD_CHECK(checks, RunAppShutdown(AppExitPath::Normal, hooks) == AppShutdownResult::Completed);
    CHILD_CHECK(checks, !AppIpc::TrySendPath(name, L"C:\\b.png", decision));

    // The silent client's pipe was closed by the server.
    DWORD available = 0;
    CHILD_CHECK(checks, !PeekNamedPipe(silent, nullptr, 0, nullptr, &available, nullptr));
    CloseHandle(silent);
    return checks.failures == 0 ? 0 : 1;   // static teardown runs after this
}

FIC2_TEST(AppShutdown_IpcStageJoinsServerThreadsBeforeLogAndStaticTeardown)
{
    const std::filesystem::path dir = MakeTempDir(L"ipcclean");
    const ChildRun run = RunChild("AppShutdownIpcCleanExitChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == 0);
    FIC2_CHECK(std::filesystem::exists(dir / L"log.txt"));
    FIC2_CHECK(std::filesystem::exists(dir / L"static-teardown.txt"));

    // No IPC line was logged after the shutdown began.
    const std::string log = ReadLog(dir, run.processId);
    const size_t shutdown = log.find("=== FICture2 shutdown (normal exit) ===");
    FIC2_CHECK(log.find("[IPC] StartServer") != std::string::npos);
    FIC2_CHECK(shutdown != std::string::npos);
    FIC2_CHECK(shutdown == std::string::npos || log.find("[IPC]", shutdown) == std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FIC2_CHILD(AppShutdownStuckIpcHandlerChild)
{
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = ChildDirArgument();
    if (dir.empty())
    {
        return 3;
    }
    static StaticTeardownSentinel sentinel { dir / L"static-teardown.txt" };
    AppLog::Init(dir.wstring());

    // Never destroyed: the stuck worker keeps using them until the process ends.
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto* server = new AppIpc::Server();
    const std::wstring name = ChildPipeName();
    const bool started = server->Start(name, [entered, never](const std::wstring&)
    {
        SetEvent(entered);
        WaitForSingleObject(never, INFINITE);
        return AppIpc::Decision::Ignore;
    });
    if (!started)
    {
        return 4;
    }
    std::thread([name]
    {
        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        (void)AppIpc::TrySendPath(name, L"C:\\a.png", decision);
    }).detach();
    if (WaitForSingleObject(entered, 10000) != WAIT_OBJECT_0)
    {
        return 4;
    }

    AppShutdownHooks hooks = DefaultAppShutdownHooks(
        false, [dir] { Touch(dir / L"before-forced.txt"); }, Floar::ArchiveReaderCache::Shared(), server);
    hooks.finishFolderEnumeration = [dir](std::chrono::milliseconds)
    {
        Touch(dir / L"enumeration.txt");
        return true;
    };
    UseMarkerTeardown(hooks, dir);
    AppShutdownBounds bounds {};
    bounds.ipcServer = kChildBound;
    RunAppShutdown(AppExitPath::Normal, hooks, bounds);
    return 5;  // not reached: the forced path ends the process
}

FIC2_TEST(AppShutdownForcedExit_StuckIpcHandlerEndsProcessAtIpcStage)
{
    const std::filesystem::path dir = MakeTempDir(L"stuckipc");
    const ChildRun run = RunChild("AppShutdownStuckIpcHandlerChild", dir);
    FIC2_CHECK(run.finished);
    FIC2_CHECK(run.exitCode == kIpcServerForcedExitCode);
    FIC2_CHECK(run.elapsed >= kChildBound);

    FIC2_CHECK(std::filesystem::exists(dir / L"before-forced.txt"));
    // No later stage and no static destructor ran beside the stuck worker.
    FIC2_CHECK(!std::filesystem::exists(dir / L"enumeration.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"gpu.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"fd2d.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"log.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"ole.txt"));
    FIC2_CHECK(!std::filesystem::exists(dir / L"static-teardown.txt"));

    const std::string log = ReadLog(dir, run.processId);
    FIC2_CHECK(log.find("IPC server still running 300 ms after shutdown") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
