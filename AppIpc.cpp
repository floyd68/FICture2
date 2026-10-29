#include "AppIpc.h"
#include "AppLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr std::uint32_t kProtocolVersion = 2; // v2: Waiting ack precedes the final Decision
    // Distinct from every Decision value; tells the client "request accepted,
    // final decision follows - keep waiting".
    constexpr std::uint32_t kWaitingAck = 0xFFFFFFFFu;

    // Staged client hard timeouts (see AppIpc.h's protocol comment for the
    // full ladder).
    constexpr DWORD kRequestResponseTimeoutMs = 500; // request sent -> first server message
    constexpr DWORD kPostAckTimeoutMs = 2000;        // silence allowed after each Waiting ack
    // Cap Waiting-ack parking: a healthy queue-or-decline server answers after
    // the first ack; more than this many means it is alive but not deciding.
    constexpr int kMaxWaitingAcks = 3;

    constexpr DWORD kCreatePipeRetryMs = 250;

    DWORD TimeoutUntil(Clock::time_point deadline)
    {
        const Clock::time_point now = Clock::now();
        if (deadline <= now)
        {
            return 0;
        }
        const long long ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<DWORD>((std::min)(ms, static_cast<long long>(INFINITE - 1)));
    }

    // Waits for the overlapped operation `ov` started on `h`. Fails when the
    // operation failed, `timeoutMs` passed or `stopEvent` (optional) was
    // signaled; then the operation is canceled and completed first, so `ov`
    // may safely leave scope.
    bool FinishOverlapped(HANDLE h, OVERLAPPED& ov, HANDLE stopEvent, DWORD timeoutMs, DWORD& transferred)
    {
        const HANDLE waits[] { ov.hEvent, stopEvent };
        const DWORD count = (stopEvent != nullptr) ? 2 : 1;
        if (WaitForMultipleObjects(count, waits, FALSE, timeoutMs) != WAIT_OBJECT_0)
        {
            CancelIoEx(h, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(h, &ov, &ignored, TRUE);
            return false;
        }
        return GetOverlappedResult(h, &ov, &transferred, FALSE) != FALSE;
    }

    // Deadline-bounded I/O on a FILE_FLAG_OVERLAPPED pipe handle, on both
    // ends: a plain blocking ReadFile could hang forever on a frozen server
    // or a silent client, which is exactly what the staged client timeouts
    // and the server's stop event exist to prevent. Handles partial
    // transfers (byte-mode pipe) by looping under one deadline.
    bool OverlappedIo(HANDLE h, HANDLE event, HANDLE stopEvent, bool isWrite, void* data, DWORD bytes,
        Clock::time_point deadline)
    {
        std::uint8_t* p = static_cast<std::uint8_t*>(data);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            OVERLAPPED ov {};
            ov.hEvent = event;
            ResetEvent(event);
            const BOOL started = isWrite
                ? WriteFile(h, p, remaining, nullptr, &ov)
                : ReadFile(h, p, remaining, nullptr, &ov);
            if (!started && GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }

            DWORD transferred = 0;
            if (!FinishOverlapped(h, ov, stopEvent, TimeoutUntil(deadline), transferred) || transferred == 0)
            {
                return false;
            }
            p += transferred;
            remaining -= transferred;
        }
        return true;
    }

    bool ClientIo(HANDLE h, HANDLE event, bool isWrite, void* data, DWORD bytes, DWORD timeoutMs)
    {
        return OverlappedIo(h, event, nullptr, isWrite, data, bytes,
            Clock::now() + std::chrono::milliseconds(timeoutMs));
    }

    bool HandleOneClient(HANDLE pipe, HANDLE ioEvent, HANDLE stopEvent, Clock::time_point deadline,
        const AppIpc::RequestHandler& onRequest)
    {
        auto io = [&](bool isWrite, void* data, DWORD bytes)
        {
            return OverlappedIo(pipe, ioEvent, stopEvent, isWrite, data, bytes, deadline);
        };

        std::uint32_t version = 0;
        std::uint32_t payloadBytes = 0;
        if (!io(false, &version, sizeof(version)) || !io(false, &payloadBytes, sizeof(payloadBytes)))
        {
            return false;
        }

        // Cap the payload to something sane for "one file path" so a
        // malformed/hostile client cannot make the server allocate wildly.
        constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;
        if (version != kProtocolVersion || payloadBytes == 0 || payloadBytes > kMaxPayloadBytes ||
            (payloadBytes % sizeof(wchar_t)) != 0)
        {
            return false;
        }

        std::vector<wchar_t> buf(payloadBytes / sizeof(wchar_t));
        if (!io(false, buf.data(), payloadBytes))
        {
            return false;
        }

        // Ensure null-termination (client sends a null-terminated string).
        if (buf.empty() || buf.back() != L'\0')
        {
            buf.push_back(L'\0');
        }

        const std::wstring path(buf.data());

        // Ack receipt before consulting the app. onRequest is a fast
        // queue-or-decline check, so the decision follows within milliseconds.
        std::uint32_t ack = kWaitingAck;
        if (!io(true, &ack, sizeof(ack)))
        {
            return false;
        }

        const AppIpc::Decision decision = onRequest ? onRequest(path) : AppIpc::Decision::Ignore;

        std::uint32_t resp = static_cast<std::uint32_t>(decision);
        return io(true, &resp, sizeof(resp));
    }

    struct ClientWorker
    {
        std::thread thread {};
        bool done { false };   // guarded by State::mutex; set last
    };
}

struct AppIpc::Server::State
{
    HANDLE stopEvent { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    // Auto-reset: a client worker finished, so the accept thread may listen again.
    HANDLE slotFreed { CreateEventW(nullptr, FALSE, FALSE, nullptr) };

    // Set by Start before the accept thread runs; read-only afterwards.
    std::wstring pipeName {};
    RequestHandler onRequest {};

    mutable std::mutex mutex;
    bool started { false };
    bool stopRequested { false };
    std::thread acceptThread {};
    std::list<ClientWorker> clients {};   // stable addresses; joined by the accept thread and Stop
    std::size_t liveWorkers { 0 };
    std::size_t liveClients { 0 };

    ~State()
    {
        if (stopEvent != nullptr)
        {
            CloseHandle(stopEvent);
        }
        if (slotFreed != nullptr)
        {
            CloseHandle(slotFreed);
        }
    }

    bool Stopping() const
    {
        return WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
    }

    void ServeClient(HANDLE pipe, ClientWorker& self)
    {
        HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ioEvent != nullptr)
        {
            const Clock::time_point deadline = Clock::now() + kClientExchangeTimeout;
            (void)HandleOneClient(pipe, ioEvent, stopEvent, deadline, onRequest);
            CloseHandle(ioEvent);
        }
        // Close without DisconnectNamedPipe: disconnecting discards a decision
        // the client has not read yet (the request is already queued), while a
        // closed instance keeps it readable. Instances are never reused.
        CloseHandle(pipe);

        std::lock_guard<std::mutex> lock(mutex);
        self.done = true;
        --liveClients;
        --liveWorkers;
        SetEvent(slotFreed);
    }

    void ReapFinishedClients()
    {
        std::list<ClientWorker> finished;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = clients.begin(); it != clients.end();)
            {
                const auto next = std::next(it);
                if (it->done)
                {
                    finished.splice(finished.end(), clients, it);
                }
                it = next;
            }
        }
        for (ClientWorker& worker : finished)
        {
            worker.thread.join();   // past its last statement
        }
    }

    // False when the server stops, or when no client worker could start.
    bool StartClient(HANDLE pipe)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopRequested)
        {
            CloseHandle(pipe);
            return false;
        }
        ClientWorker& worker = clients.emplace_back();
        try
        {
            worker.thread = std::thread([this, pipe, &worker] { ServeClient(pipe, worker); });
        }
        catch (const std::system_error&)
        {
            clients.pop_back();
            CloseHandle(pipe);
            FIC2_LOG_ERROR("[IPC] Server: could not start a client worker.");
            return false;
        }
        ++liveClients;
        ++liveWorkers;
        return true;
    }

    // Waits for a client to connect to `pipe`; false on stop or failure.
    bool WaitForClient(HANDLE pipe, HANDLE connectEvent)
    {
        OVERLAPPED ov {};
        ov.hEvent = connectEvent;
        ResetEvent(connectEvent);
        if (ConnectNamedPipe(pipe, &ov))
        {
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED)
        {
            return true;
        }
        if (err == ERROR_IO_PENDING)
        {
            DWORD ignored = 0;
            return FinishOverlapped(pipe, ov, stopEvent, INFINITE, ignored);
        }
        FIC2_LOG_WARN("[IPC] Server: ConnectNamedPipe failed (err={}), discarding pipe.", err);
        return false;
    }

    void AcceptLoop(HANDLE connectEvent)
    {
        for (;;)
        {
            // Listen only while a client slot is free; waiting clients queue
            // in WaitNamedPipeW meanwhile.
            for (;;)
            {
                ReapFinishedClients();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (liveClients < kMaxClientWorkers)
                    {
                        break;
                    }
                }
                const HANDLE waits[] { stopEvent, slotFreed };
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
                {
                    return;
                }
            }
            if (Stopping())
            {
                return;
            }

            HANDLE pipe = CreateNamedPipeW(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024,
                64 * 1024,
                0,
                nullptr);

            if (pipe == INVALID_HANDLE_VALUE)
            {
                FIC2_LOG_ERROR("[IPC] Server: CreateNamedPipeW failed (err={}), retrying in {}ms.",
                    GetLastError(), kCreatePipeRetryMs);
                if (WaitForSingleObject(stopEvent, kCreatePipeRetryMs) != WAIT_TIMEOUT)
                {
                    return;
                }
                continue;
            }

            if (!WaitForClient(pipe, connectEvent))
            {
                CloseHandle(pipe);
                if (Stopping())
                {
                    return;
                }
                continue;
            }

            // Per-client worker, so a slow or silent client does not hold up
            // consecutive Explorer opens (each has a 500ms connect budget).
            if (!StartClient(pipe) && Stopping())
            {
                return;
            }
        }
    }

    void AcceptMain()
    {
        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connectEvent != nullptr)
        {
            AcceptLoop(connectEvent);
            CloseHandle(connectEvent);
        }
        else
        {
            FIC2_LOG_ERROR("[IPC] Server: CreateEventW failed (err={}).", GetLastError());
        }

        std::lock_guard<std::mutex> lock(mutex);
        --liveWorkers;
    }
};

namespace AppIpc
{
    Server::Server()
        : m_state(std::make_shared<State>())
    {
    }

    Server::~Server()
    {
        while (!Stop(std::chrono::hours(1)))
        {
        }
    }

    bool Server::Start(const std::wstring& pipeName, RequestHandler onRequest)
    {
        State& state = *m_state;
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started || state.stopRequested || state.stopEvent == nullptr || state.slotFreed == nullptr)
        {
            return false;
        }
        state.pipeName = pipeName;
        state.onRequest = std::move(onRequest);
        try
        {
            state.acceptThread = std::thread([&state] { state.AcceptMain(); });
        }
        catch (const std::system_error&)
        {
            FIC2_LOG_ERROR("[IPC] StartServer: could not start the accept thread.");
            return false;
        }
        state.started = true;
        ++state.liveWorkers;
        FIC2_LOG_INFO("[IPC] StartServer: named-pipe server thread started.");
        return true;
    }

    bool Server::Stop(std::chrono::milliseconds bound)
    {
        const Clock::time_point deadline = Clock::now() + bound;
        State& state = *m_state;
        std::thread accept;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stopRequested = true;
            accept = std::move(state.acceptThread);
        }
        if (state.stopEvent != nullptr)
        {
            SetEvent(state.stopEvent);
        }

        if (accept.joinable())
        {
            if (WaitForSingleObject(static_cast<HANDLE>(accept.native_handle()), TimeoutUntil(deadline)) != WAIT_OBJECT_0)
            {
                FIC2_LOG_WARN("[IPC] Server: the accept thread has not exited at shutdown.");
                std::lock_guard<std::mutex> lock(state.mutex);
                state.acceptThread = std::move(accept);
                return false;
            }
            accept.join();
        }

        // The accept thread is gone, so only the client workers touch the list
        // now; splicing keeps the nodes they refer to.
        std::list<ClientWorker> clients;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            clients.splice(clients.end(), state.clients);
        }
        for (auto it = clients.begin(); it != clients.end();)
        {
            if (WaitForSingleObject(static_cast<HANDLE>(it->thread.native_handle()), TimeoutUntil(deadline)) == WAIT_OBJECT_0)
            {
                it->thread.join();
                it = clients.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (clients.empty())
        {
            return true;
        }

        // Kept, not detached: a later Stop can still join them.
        FIC2_LOG_WARN("[IPC] Server: {} client worker(s) have not exited at shutdown.", clients.size());
        std::lock_guard<std::mutex> lock(state.mutex);
        state.clients.splice(state.clients.end(), clients);
        return false;
    }

    std::size_t Server::WorkerCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->liveWorkers;
    }

    std::size_t Server::ClientWorkerCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->liveClients;
    }

    bool TrySendPath(const std::wstring& pipeName, const std::wstring& path, Decision& outDecision)
    {
        outDecision = Decision::Ignore;

        if (path.empty())
        {
            return false;
        }

        // Connect to the server pipe with retry logic.
        //
        // Two distinct failure modes:
        //   ERROR_FILE_NOT_FOUND - pipe not yet created (server thread still
        //                          starting); WaitNamedPipeW returns
        //                          immediately for this, so poll instead.
        //   ERROR_PIPE_BUSY      - pipe exists but occupied; WaitNamedPipeW
        //                          blocks until an instance frees up.
        constexpr DWORD kTotalBudgetMs = 500;
        constexpr DWORD kPollIntervalMs = 5;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kTotalBudgetMs);
        int retryCount = 0;

        HANDLE h = INVALID_HANDLE_VALUE;
        while (std::chrono::steady_clock::now() < deadline)
        {
            // FILE_FLAG_OVERLAPPED: all pipe I/O below runs through
            // ClientIo so each stage carries its own hard timeout.
            h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                break;
            }

            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
            {
                ++retryCount;
                Sleep(kPollIntervalMs);
                continue;
            }
            if (err == ERROR_PIPE_BUSY)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining > 0 && WaitNamedPipeW(pipeName.c_str(), static_cast<DWORD>(remaining)))
                {
                    continue;
                }
                const DWORD waitErr = GetLastError();
                if (remaining > 0 && waitErr == ERROR_FILE_NOT_FOUND)
                {
                    // Every instance closed at once (served clients leaving
                    // while the server is between listens): poll as above.
                    ++retryCount;
                    Sleep(kPollIntervalMs);
                    continue;
                }
                FIC2_LOG_WARN("[IPC] Client: WaitNamedPipeW timed out (err={}).", waitErr);
                break;
            }
            FIC2_LOG_ERROR("[IPC] Client: pipe connect failed (err={}).", err);
            break;
        }

        if (h == INVALID_HANDLE_VALUE)
        {
            if (retryCount > 0)
            {
                FIC2_LOG_WARN("[IPC] Client: server pipe not available after {}ms ({} retries).",
                    kTotalBudgetMs, retryCount);
            }
            return false;
        }

        HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ioEvent == nullptr)
        {
            CloseHandle(h);
            return false;
        }

        std::wstring payload = path + L'\0';
        std::uint32_t version = kProtocolVersion;
        std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload.size() * sizeof(wchar_t));

        // Stage 2: the whole request plus the first server message must fit
        // in kRequestResponseTimeoutMs each - a server that accepted the
        // connection but never acks is not actually serving.
        bool ok = true;
        ok = ok && ClientIo(h, ioEvent, true, &version, sizeof(version), kRequestResponseTimeoutMs);
        ok = ok && ClientIo(h, ioEvent, true, &payloadBytes, sizeof(payloadBytes), kRequestResponseTimeoutMs);
        ok = ok && ClientIo(h, ioEvent, true, payload.data(), payloadBytes, kRequestResponseTimeoutMs);

        std::uint32_t resp = 0;
        ok = ok && ClientIo(h, ioEvent, false, &resp, sizeof(resp), kRequestResponseTimeoutMs);

        // Stage 3: parked on Waiting acks, doubly bounded. Silence past
        // kPostAckTimeoutMs after an ack means the server froze or died;
        // more than kMaxWaitingAcks acks means it is alive but not deciding.
        int acksSeen = 0;
        while (ok && resp == kWaitingAck)
        {
            if (++acksSeen > kMaxWaitingAcks)
            {
                FIC2_LOG_WARN("[IPC] Client: server still undecided after {} Waiting acks - proceeding alone.",
                    kMaxWaitingAcks);
                ok = false;
                break;
            }
            ok = ClientIo(h, ioEvent, false, &resp, sizeof(resp), kPostAckTimeoutMs);
        }

        CloseHandle(ioEvent);
        CloseHandle(h);

        if (!ok)
        {
            FIC2_LOG_ERROR("[IPC] Client: pipe I/O failed or timed out during send/recv.");
            return false;
        }

        outDecision = static_cast<Decision>(resp);
        FIC2_LOG_INFO("[IPC] Client: server responded with decision={}.", static_cast<int>(outDecision));
        return true;
    }
}
