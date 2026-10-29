#include "TestHarness.h"

#include "AppIpc.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The owned IPC server (AppIpc::Server): requests round-trip, Stop joins an
// idle server and one with silent clients within its bound, client workers are
// capped, a silent client is dropped after the exchange timeout, and a handler
// that does not return makes Stop report failure until it does. Every test
// uses its own pipe name, never the app's, so a running FICture2 is not
// reached.

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    std::wstring UniquePipeName()
    {
        static std::atomic<unsigned> counter { 0 };
        return L"\\\\.\\pipe\\FICture2_IPC_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(counter.fetch_add(1));
    }

    bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s)
    {
        const auto deadline = Clock::now() + timeout;
        while (!condition())
        {
            if (Clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    std::chrono::milliseconds Since(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    }

    // A client that connects and sends nothing (or only `prefix`).
    struct RawClient
    {
        HANDLE pipe { INVALID_HANDLE_VALUE };

        RawClient() = default;
        RawClient(const RawClient&) = delete;
        RawClient& operator=(const RawClient&) = delete;
        ~RawClient() { Close(); }

        bool Connect(const std::wstring& name, std::chrono::milliseconds timeout = 5s)
        {
            const auto deadline = Clock::now() + timeout;
            while (Clock::now() < deadline)
            {
                pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                {
                    return true;
                }
                // Not listening yet (accept thread starting or between clients).
                (void)WaitNamedPipeW(name.c_str(), 50);
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }

        bool Send(const void* data, DWORD bytes)
        {
            DWORD written = 0;
            return WriteFile(pipe, data, bytes, &written, nullptr) && written == bytes;
        }

        // The server disconnected or closed its end (no blocking read).
        bool Broken() const
        {
            DWORD available = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                return false;
            }
            const DWORD err = GetLastError();
            return err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_BROKEN_PIPE;
        }

        void Close()
        {
            if (pipe != INVALID_HANDLE_VALUE)
            {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
        }
    };

    struct RecordingHandler
    {
        std::mutex mutex;
        std::vector<std::wstring> paths;
        AppIpc::Decision decision { AppIpc::Decision::CompareStarted };

        AppIpc::RequestHandler Make()
        {
            return [this](const std::wstring& path)
            {
                std::lock_guard<std::mutex> lock(mutex);
                paths.push_back(path);
                return decision;
            };
        }

        size_t Count()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return paths.size();
        }
    };
}

FIC2_TEST(AppIpc_RequestsRoundTripOnAUniquePipe)
{
    const std::wstring name = UniquePipeName();
    FIC2_CHECK(name != AppIpc::kPipeName);

    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));
    FIC2_CHECK(!server.Start(name, handler.Make()));   // one start per server

    AppIpc::Decision decision = AppIpc::Decision::Ignore;
    const std::wstring path = L"C:\\pictures\\\u00e9t\u00e9 \u5199\u771f.png";
    FIC2_CHECK(AppIpc::TrySendPath(name, path, decision));
    FIC2_CHECK(decision == AppIpc::Decision::CompareStarted);

    handler.decision = AppIpc::Decision::Ignore;
    FIC2_CHECK(AppIpc::TrySendPath(name, L"C:\\other\\b.png", decision));
    FIC2_CHECK(decision == AppIpc::Decision::Ignore);

    FIC2_REQUIRE(handler.Count() == 2);
    FIC2_CHECK(handler.paths[0] == path);
    FIC2_CHECK(handler.paths[1] == L"C:\\other\\b.png");

    FIC2_CHECK(server.Stop(AppIpc::kClientExchangeTimeout));
    FIC2_CHECK(server.WorkerCount() == 0);
}

FIC2_TEST(AppIpc_RapidConcurrentClientsAreEachAnsweredOnce)
{
    const std::wstring name = UniquePipeName();
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));

    // More clients than worker slots, all at once (a multi-select Explorer open).
    constexpr int kClients = static_cast<int>(AppIpc::kMaxClientWorkers) + 4;
    std::atomic<int> answered { 0 };
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i)
    {
        clients.emplace_back([&, i]
        {
            AppIpc::Decision decision = AppIpc::Decision::Ignore;
            if (AppIpc::TrySendPath(name, L"C:\\f\\" + std::to_wstring(i) + L".png", decision) &&
                decision == AppIpc::Decision::CompareStarted)
            {
                answered.fetch_add(1);
            }
        });
    }
    for (std::thread& client : clients)
    {
        client.join();
    }
    FIC2_CHECK(answered.load() == kClients);
    // No request lost or handled twice.
    const std::set<std::wstring> distinct(handler.paths.begin(), handler.paths.end());
    FIC2_CHECK(handler.Count() == static_cast<size_t>(kClients));
    FIC2_CHECK(distinct.size() == static_cast<size_t>(kClients));

    FIC2_CHECK(server.Stop(AppIpc::kClientExchangeTimeout));
    FIC2_CHECK(server.WorkerCount() == 0);
}

FIC2_TEST(AppIpc_IdleServerStopsWithinTheBound)
{
    const std::wstring name = UniquePipeName();
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));
    // The accept thread is waiting for a client.
    FIC2_REQUIRE(WaitUntil([&] { return WaitNamedPipeW(name.c_str(), 0) != FALSE; }));
    FIC2_CHECK(server.WorkerCount() == 1);

    const auto start = Clock::now();
    FIC2_CHECK(server.Stop(1000ms));
    FIC2_CHECK(Since(start) < 500ms);
    FIC2_CHECK(server.WorkerCount() == 0);

    // The pipe is gone: a client finds no server and runs alone.
    AppIpc::Decision decision = AppIpc::Decision::CompareStarted;
    FIC2_CHECK(!AppIpc::TrySendPath(name, L"C:\\a.png", decision));
    FIC2_CHECK(decision == AppIpc::Decision::Ignore);
    FIC2_CHECK(handler.Count() == 0);

    // Stop is repeatable and the server does not start again.
    FIC2_CHECK(server.Stop(0ms));
    FIC2_CHECK(!server.Start(name, handler.Make()));
}

FIC2_TEST(AppIpc_StopBeforeStartSucceedsAndPreventsStart)
{
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_CHECK(server.Stop(0ms));
    FIC2_CHECK(server.WorkerCount() == 0);
    FIC2_CHECK(!server.Start(UniquePipeName(), handler.Make()));
    FIC2_CHECK(server.WorkerCount() == 0);
}

FIC2_TEST(AppIpc_SilentClientsDoNotDelayStop)
{
    const std::wstring name = UniquePipeName();
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));

    // One client sends nothing, one stops after the protocol version.
    RawClient silent;
    FIC2_REQUIRE(silent.Connect(name));
    FIC2_REQUIRE(WaitUntil([&] { return server.ClientWorkerCount() == 1; }));
    RawClient partial;
    FIC2_REQUIRE(partial.Connect(name));
    const std::uint32_t version = 2;
    FIC2_REQUIRE(partial.Send(&version, sizeof(version)));
    FIC2_REQUIRE(WaitUntil([&] { return server.ClientWorkerCount() == 2; }));
    FIC2_CHECK(server.WorkerCount() == 3);

    const auto start = Clock::now();
    FIC2_CHECK(server.Stop(1000ms));
    // Well inside the exchange timeout: the stop event woke the reads.
    FIC2_CHECK(Since(start) < 500ms);
    FIC2_CHECK(server.WorkerCount() == 0);
    FIC2_CHECK(server.ClientWorkerCount() == 0);
    FIC2_CHECK(silent.Broken());
    FIC2_CHECK(partial.Broken());
    FIC2_CHECK(handler.Count() == 0);
}

FIC2_TEST(AppIpc_SilentClientIsDroppedAfterTheExchangeTimeout)
{
    const std::wstring name = UniquePipeName();
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));

    RawClient silent;
    FIC2_REQUIRE(silent.Connect(name));
    FIC2_REQUIRE(WaitUntil([&] { return server.ClientWorkerCount() == 1; }));
    const auto connected = Clock::now();
    FIC2_CHECK(WaitUntil([&] { return server.ClientWorkerCount() == 0; }, AppIpc::kClientExchangeTimeout + 3s));
    FIC2_CHECK(Since(connected) >= AppIpc::kClientExchangeTimeout - 100ms);
    FIC2_CHECK(silent.Broken());

    // The server keeps serving.
    AppIpc::Decision decision = AppIpc::Decision::Ignore;
    FIC2_CHECK(AppIpc::TrySendPath(name, L"C:\\a.png", decision));
    FIC2_CHECK(decision == AppIpc::Decision::CompareStarted);
    FIC2_CHECK(server.Stop(1000ms));
}

FIC2_TEST(AppIpc_ClientWorkersAreBounded)
{
    const std::wstring name = UniquePipeName();
    RecordingHandler handler;
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, handler.Make()));

    std::vector<std::unique_ptr<RawClient>> clients;
    for (size_t i = 0; i < AppIpc::kMaxClientWorkers; ++i)
    {
        clients.push_back(std::make_unique<RawClient>());
        FIC2_REQUIRE(clients.back()->Connect(name));
        FIC2_REQUIRE(WaitUntil([&] { return server.ClientWorkerCount() == i + 1; }));
    }
    // Every slot is taken: nothing listens, so the next client waits.
    FIC2_CHECK(!WaitNamedPipeW(name.c_str(), 300));
    FIC2_CHECK(server.ClientWorkerCount() == AppIpc::kMaxClientWorkers);
    FIC2_CHECK(server.WorkerCount() == AppIpc::kMaxClientWorkers + 1);

    // A client that leaves frees its slot and the waiting request is served.
    clients.front()->Close();
    AppIpc::Decision decision = AppIpc::Decision::Ignore;
    FIC2_CHECK(AppIpc::TrySendPath(name, L"C:\\a.png", decision));
    FIC2_CHECK(decision == AppIpc::Decision::CompareStarted);
    FIC2_CHECK(server.ClientWorkerCount() <= AppIpc::kMaxClientWorkers);

    const auto start = Clock::now();
    FIC2_CHECK(server.Stop(1000ms));
    FIC2_CHECK(Since(start) < 500ms);
    FIC2_CHECK(server.WorkerCount() == 0);
}

FIC2_TEST(AppIpc_StuckHandlerFailsStopUntilItReturns)
{
    const std::wstring name = UniquePipeName();
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    AppIpc::Server server;
    FIC2_REQUIRE(server.Start(name, [&](const std::wstring&)
    {
        SetEvent(entered);
        WaitForSingleObject(release, INFINITE);
        return AppIpc::Decision::Ignore;
    }));

    std::thread client([&]
    {
        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        (void)AppIpc::TrySendPath(name, L"C:\\a.png", decision);
    });
    FIC2_CHECK(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0);

    const auto start = Clock::now();
    FIC2_CHECK(!server.Stop(200ms));
    const auto elapsed = Since(start);
    FIC2_CHECK(elapsed >= 200ms - 20ms);   // a timed wait may end a clock tick early
    FIC2_CHECK(elapsed < 1000ms);
    // The accept thread joined; the stuck client worker is kept, not detached.
    FIC2_CHECK(server.WorkerCount() == 1);
    FIC2_CHECK(server.ClientWorkerCount() == 1);

    SetEvent(release);
    FIC2_CHECK(server.Stop(5000ms));
    FIC2_CHECK(server.WorkerCount() == 0);
    client.join();
    CloseHandle(entered);
    CloseHandle(release);
}
