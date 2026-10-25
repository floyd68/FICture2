#include "TestHarness.h"

#include "AppLog.h"
#include "FolderEnumeration.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Folder enumeration sessions: a global worker cap that holds while calls are
// blocked, latest-request-wins per browser, detaching without waiting, and the
// normal (join within a bound) and forced (end the process) shutdown paths.

namespace
{
    using namespace std::chrono_literals;

    // Blocks every enumeration until Release(); with honourStop it also returns
    // when the session's stop is requested (cooperative cancellation).
    class BlockingBackend final : public FolderEnumerationBackend
    {
    public:
        explicit BlockingBackend(bool honourStop)
            : m_honourStop(honourStop)
        {
        }

        void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) override
        {
            // Registered before taking the lock: it runs inline if stop is already requested.
            std::stop_callback wake(stop, [this] { std::lock_guard<std::mutex> lock(m_mutex); m_changed.notify_all(); });
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_entered.push_back(folder.wstring());
                m_maxActive = (std::max)(m_maxActive, ++m_active);
                m_changed.notify_all();
                m_changed.wait(lock, [&] { return m_released || (m_honourStop && stop.stop_requested()); });
                --m_active;
            }
            std::vector<Floar::VirtualFileEntry> entries {};
            entries.emplace_back(Floar::VirtualPath(folder.hostPath / L"a.png"), false, 0, 0);
            sink(std::move(entries), true);
        }

        void Release()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_changed.notify_all();
        }

        bool WaitEntered(size_t count, std::chrono::milliseconds timeout = 5s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_entered.size() >= count; });
        }

        std::vector<std::wstring> Entered()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_entered;
        }

        size_t MaxActive()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_maxActive;
        }

    private:
        const bool m_honourStop;
        std::mutex m_mutex {};
        std::condition_variable m_changed {};
        std::vector<std::wstring> m_entered {};
        size_t m_active { 0 };
        size_t m_maxActive { 0 };
        bool m_released { false };
    };

    FolderLoadRequest Request(unsigned long long id, const wchar_t* folder)
    {
        FolderLoadRequest request {};
        request.requestId = id;
        request.folder = Floar::VirtualPath(std::filesystem::path(L"C:\\enum-test") / folder);
        return request;
    }

    bool WaitForChunks(FolderLoadLane& lane, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!lane.HasChunks())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    std::filesystem::path MakeTempDir(const wchar_t* tag)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
            (std::wstring(L"fic2-") + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(dir);
        return dir;
    }
}

FIC2_TEST(FolderEnumeration_WorkerCapHoldsWhileCallsBlock)
{
    auto backend = std::make_shared<BlockingBackend>(false);
    FolderEnumerationService service(backend, 2);
    FolderLoadLane a = service.CreateLane();
    FolderLoadLane b = service.CreateLane();
    FolderLoadLane c = service.CreateLane();

    a.Start(Request(1, L"a"));
    b.Start(Request(1, L"b"));
    c.Start(Request(1, L"c"));
    FIC2_REQUIRE(backend->WaitEntered(2));

    // Both workers are blocked: the third request waits for a slot.
    std::this_thread::sleep_for(100ms);
    FIC2_CHECK(backend->Entered().size() == 2);
    FIC2_CHECK(service.WorkerCount() == 2);
    FIC2_CHECK(service.RunningCount() == 2);

    backend->Release();
    FIC2_CHECK(WaitForChunks(a) && WaitForChunks(b) && WaitForChunks(c));
    FIC2_CHECK(backend->MaxActive() == 2);
    FIC2_CHECK(service.WorkerCount() == 2);

    FIC2_CHECK(service.Shutdown(5s));
    FIC2_CHECK(service.WorkerCount() == 0);
}

FIC2_TEST(FolderEnumeration_LatestPendingRequestWins)
{
    auto backend = std::make_shared<BlockingBackend>(false);
    FolderEnumerationService service(backend, 1);
    FolderLoadLane lane = service.CreateLane();

    lane.Start(Request(1, L"first"));
    FIC2_REQUIRE(backend->WaitEntered(1));
    // While the first call is blocked, two more navigations: only the latest runs.
    lane.Start(Request(2, L"second"));
    lane.Start(Request(3, L"third"));
    backend->Release();

    FIC2_REQUIRE(WaitForChunks(lane));
    FIC2_CHECK(service.Shutdown(5s));
    const std::vector<std::wstring> entered = backend->Entered();
    FIC2_REQUIRE(entered.size() == 2);
    FIC2_CHECK(entered[0].ends_with(L"first"));
    FIC2_CHECK(entered[1].ends_with(L"third"));

    // The superseded first result never reaches the browser.
    const std::deque<AsyncThumbListChunkPayload> chunks = lane.Dequeue();
    FIC2_REQUIRE(chunks.size() == 1);
    FIC2_CHECK(chunks.front().requestId == 3);
    FIC2_CHECK(chunks.front().completed);
    FIC2_CHECK(chunks.front().folder.hostPath.filename() == L"third");
}

FIC2_TEST(FolderEnumeration_CloseNeverWaitsForABlockedCall)
{
    auto backend = std::make_shared<BlockingBackend>(false);
    FolderEnumerationService service(backend, 2);
    FolderLoadLane lane = service.CreateLane();
    lane.Start(Request(1, L"slow"));
    FIC2_REQUIRE(backend->WaitEntered(1));

    // Browser destruction while the call is blocked: returns at once.
    const auto start = std::chrono::steady_clock::now();
    lane.Close();
    FIC2_CHECK(std::chrono::steady_clock::now() - start < 50ms);
    FIC2_CHECK(service.RunningCount() == 1);

    // The worker is still blocked: a bounded shutdown reports it (no detach).
    FIC2_CHECK(!service.Shutdown(100ms));
    FIC2_CHECK(service.WorkerCount() == 1);

    // Once the call returns, its result is dropped and the worker joins.
    backend->Release();
    FIC2_CHECK(service.Shutdown(5s));
    FIC2_CHECK(service.WorkerCount() == 0);
    FIC2_CHECK(!lane.HasChunks());
}

FIC2_TEST(FolderEnumeration_ShutdownCancelsAndJoins)
{
    auto backend = std::make_shared<BlockingBackend>(true);
    FolderEnumerationService service(backend, 2);
    FolderLoadLane a = service.CreateLane();
    FolderLoadLane b = service.CreateLane();
    FolderLoadLane c = service.CreateLane();
    a.Start(Request(1, L"a"));
    b.Start(Request(1, L"b"));
    c.Start(Request(1, L"c"));  // pending: dropped by shutdown
    FIC2_REQUIRE(backend->WaitEntered(2));

    // Never released: stop requests end both calls cooperatively.
    FIC2_CHECK(service.Shutdown(5s));
    FIC2_CHECK(service.WorkerCount() == 0);
    FIC2_CHECK(backend->Entered().size() == 2);
    FIC2_CHECK(!a.HasChunks() && !b.HasChunks() && !c.HasChunks());

    // Requests after shutdown are ignored.
    a.Start(Request(2, L"late"));
    FIC2_CHECK(service.WorkerCount() == 0);
}

FIC2_TEST(FolderEnumeration_LaneDropsSupersededAndClosedChunks)
{
    auto backend = std::make_shared<BlockingBackend>(false);
    FolderEnumerationService service(backend, 1);
    FolderLoadLane lane = service.CreateLane();
    lane.Start(Request(5, L"current"));
    FIC2_REQUIRE(backend->WaitEntered(1));

    AsyncThumbListChunkPayload stale {};
    stale.requestId = 4;
    lane.Post(std::move(stale));
    FIC2_CHECK(!lane.HasChunks());

    lane.Close();
    AsyncThumbListChunkPayload late {};
    late.requestId = 5;
    lane.Post(std::move(late));
    FIC2_CHECK(!lane.HasChunks());
    FIC2_CHECK(lane.Dequeue().empty());
    // The blocked call of request 5 finishes into the closed lane.
    backend->Release();
    FIC2_CHECK(service.Shutdown(5s));
    FIC2_CHECK(!lane.HasChunks());
}

namespace
{
    // The production backend with each directory operation counted; `stopAt`
    // requests stop from inside one operation (a cancel arriving while it blocks).
    class CountingFilesystemBackend final : public FilesystemFolderEnumerationBackend
    {
    public:
        enum class Op { None, IsDirectory, OpenDirectory };

        int isDirectoryCalls { 0 };
        int openCalls { 0 };
        int advanceCalls { 0 };
        int archiveCalls { 0 };
        Op stopAt { Op::None };
        std::stop_source* stop { nullptr };

    protected:
        bool IsDirectory(const std::filesystem::path& path) override
        {
            ++isDirectoryCalls;
            StopIf(Op::IsDirectory);
            return FilesystemFolderEnumerationBackend::IsDirectory(path);
        }

        void OpenDirectory(const std::filesystem::path& path, std::filesystem::directory_iterator& it, std::error_code& ec) override
        {
            ++openCalls;
            StopIf(Op::OpenDirectory);
            FilesystemFolderEnumerationBackend::OpenDirectory(path, it, ec);
        }

        void Advance(std::filesystem::directory_iterator& it, std::error_code& ec) override
        {
            ++advanceCalls;
            FilesystemFolderEnumerationBackend::Advance(it, ec);
        }

        std::vector<Floar::VirtualFileEntry> ListArchive(const Floar::VirtualPath& folder) override
        {
            ++archiveCalls;
            return FilesystemFolderEnumerationBackend::ListArchive(folder);
        }

    private:
        void StopIf(Op op)
        {
            if (stopAt == op && stop != nullptr)
            {
                stop->request_stop();
            }
        }
    };
}

FIC2_TEST(FolderEnumeration_FilesystemBackendStartsNoOperationAfterStop)
{
    const std::filesystem::path dir = MakeTempDir(L"enum");
    for (int i = 0; i < 600; ++i)
    {
        std::ofstream(dir / (L"f" + std::to_wstring(i) + L".png")).put('x');
    }
    const Floar::VirtualPath folder(dir);
    size_t total = 0;
    int calls = 0;
    bool completed = false;
    const auto reset = [&] { total = 0; calls = 0; completed = false; };
    const auto count = [&](std::vector<Floar::VirtualFileEntry>&& batch, bool done)
    {
        total += batch.size();
        ++calls;
        completed = done;
    };

    // Full listing: 256-entry batches, the last one completed; one advance per entry.
    {
        CountingFilesystemBackend backend {};
        backend.Enumerate(folder, std::stop_token {}, count);
        FIC2_CHECK(total == 600 && calls == 3 && completed);
        FIC2_CHECK(backend.isDirectoryCalls == 1 && backend.openCalls == 1);
        FIC2_CHECK(backend.advanceCalls == 600);
    }

    // Stop requested while the first batch is delivered: the iterator is not
    // advanced again (that read could block on a network share).
    {
        reset();
        std::stop_source stop {};
        CountingFilesystemBackend backend {};
        backend.Enumerate(folder, stop.get_token(), [&](std::vector<Floar::VirtualFileEntry>&& batch, bool done)
        {
            count(std::move(batch), done);
            stop.request_stop();
        });
        FIC2_CHECK(calls == 1 && total == 256 && !completed);
        FIC2_CHECK(backend.advanceCalls == 255);   // after entries 1..255, not after the 256th
    }

    // Stop already requested: no directory or archive operation starts.
    {
        reset();
        std::stop_source stop {};
        stop.request_stop();
        CountingFilesystemBackend backend {};
        backend.Enumerate(folder, stop.get_token(), count);
        backend.Enumerate(Floar::VirtualPath(dir / L"pack.zip"), stop.get_token(), count);
        FIC2_CHECK(calls == 0);
        FIC2_CHECK(backend.isDirectoryCalls == 0 && backend.openCalls == 0 && backend.advanceCalls == 0 && backend.archiveCalls == 0);
    }

    // Stop arriving during the directory check, then during the open: the
    // next operation never starts and nothing is delivered.
    {
        reset();
        std::stop_source stop {};
        CountingFilesystemBackend backend {};
        backend.stop = &stop;
        backend.stopAt = CountingFilesystemBackend::Op::IsDirectory;
        backend.Enumerate(folder, stop.get_token(), count);
        FIC2_CHECK(backend.isDirectoryCalls == 1 && backend.openCalls == 0 && calls == 0);
    }
    {
        reset();
        std::stop_source stop {};
        CountingFilesystemBackend backend {};
        backend.stop = &stop;
        backend.stopAt = CountingFilesystemBackend::Op::OpenDirectory;
        backend.Enumerate(folder, stop.get_token(), count);
        FIC2_CHECK(backend.openCalls == 1 && backend.advanceCalls == 0 && calls == 0);
    }

    // A missing folder completes empty.
    {
        reset();
        CountingFilesystemBackend backend {};
        backend.Enumerate(Floar::VirtualPath(dir / L"missing"), std::stop_token {}, count);
        FIC2_CHECK(calls == 1 && completed && total == 0);
        FIC2_CHECK(backend.openCalls == 0);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace
{
    int DrainWakeups(HWND window)
    {
        int count = 0;
        MSG msg {};
        while (PeekMessageW(&msg, window, kFolderListingReadyMessage, kFolderListingReadyMessage, PM_REMOVE))
        {
            FIC2_CHECK(msg.wParam == 0 && msg.lParam == 0);   // no payload to own
            ++count;
        }
        return count;
    }

    // Streams one-entry chunks until stopped.
    class StreamingBackend final : public FolderEnumerationBackend
    {
    public:
        std::atomic<int> chunks { 0 };

        void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) override
        {
            while (!stop.stop_requested())
            {
                std::vector<Floar::VirtualFileEntry> entries {};
                entries.emplace_back(Floar::VirtualPath(folder.hostPath / L"a.png"), false, 0, 0);
                sink(std::move(entries), false);
                ++chunks;
            }
        }
    };
}

FIC2_TEST(FolderEnumeration_NoWakeupAfterCloseOrWindowDestroy)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    FIC2_REQUIRE(window != nullptr);
    auto wakeup = std::make_shared<FolderListingWakeup>(window);
    auto backend = std::make_shared<StreamingBackend>();
    FolderEnumerationService service(backend, 1);

    // A worker keeps delivering while the UI drains; closing the lane races it.
    FolderLoadLane lane = service.CreateLane();
    lane.SetWakeup(wakeup);
    lane.Start(Request(1, L"stream"));
    for (int i = 0; i < 200 && backend->chunks < 1000; ++i)
    {
        (void)DrainWakeups(window);
        (void)lane.Dequeue();
        std::this_thread::sleep_for(1ms);
    }
    FIC2_CHECK(backend->chunks > 0);
    lane.Close();
    (void)DrainWakeups(window);   // posted before Close returned
    std::this_thread::sleep_for(30ms);
    FIC2_CHECK(DrainWakeups(window) == 0);

    // Window destruction while a wakeup is pending: the window disconnects the
    // wakeup; later chunks still queue but post nothing, and nothing leaks.
    FolderLoadLane second = service.CreateLane();
    second.SetWakeup(wakeup);
    AsyncThumbListChunkPayload chunk {};
    second.Post(std::move(chunk));
    FIC2_CHECK(DrainWakeups(window) == 1);
    wakeup->Disconnect();
    DestroyWindow(window);
    (void)second.Dequeue();
    second.Post(AsyncThumbListChunkPayload {});
    FIC2_CHECK(second.HasChunks());
    FIC2_CHECK(!wakeup->Post());
    second.Close();

    FIC2_CHECK(service.Shutdown(5s));
}

// Child: a worker stays blocked past the shutdown bound; the forced path must
// persist state, flush the log and end the process with the documented code.
FIC2_CHILD(FolderEnumerationForcedExitChild)
{
    (void)argc;
    (void)argv;
    // The folder argument may be outside the ANSI code page: read it wide.
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv == nullptr || wideArgc < 4)
    {
        return 3;
    }
    const std::filesystem::path dir = wideArgv[wideArgc - 1];
    LocalFree(wideArgv);

    AppLog::Init(dir.wstring());
    auto backend = std::make_shared<BlockingBackend>(false);
    // Never destroyed: its destructor would wait for the blocked worker.
    auto* service = new FolderEnumerationService(backend, 1);
    FolderLoadLane lane = service->CreateLane();
    lane.Start(Request(1, L"network-share"));
    if (!backend->WaitEntered(1))
    {
        return 4;
    }

    FinishFolderEnumerationOrExit(*service, 200ms, [&]
    {
        std::ofstream(dir / L"session.ini") << "[Session]\nsaved=1\n";
    });
    return 5;  // not reached: the forced path ends the process
}

FIC2_TEST(FolderEnumerationForcedExit_SavesFlushesAndEndsProcess)
{
    const std::filesystem::path dir = MakeTempDir(L"forced");
    wchar_t exe[MAX_PATH] {};
    FIC2_REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --child FolderEnumerationForcedExitChild \"" + dir.wstring() + L"\"";

    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    FIC2_REQUIRE(CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    FIC2_CHECK(wait == WAIT_OBJECT_0);
    FIC2_CHECK(exitCode == kFolderEnumerationForcedExitCode);
    FIC2_CHECK(std::filesystem::exists(dir / L"session.ini"));

    const std::filesystem::path log = dir / (L"FICture2_" + std::to_wstring(process.dwProcessId) + L".log");
    std::ifstream logFile(log);
    const std::string text((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    FIC2_CHECK(text.find("still running 200 ms after shutdown") != std::string::npos);
    logFile.close();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
