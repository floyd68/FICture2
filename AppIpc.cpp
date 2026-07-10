#include "AppIpc.h"
#include "AppLog.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FICture2_IPC";
    constexpr uint32_t kProtocolVersion = 1;

    bool WriteAll(HANDLE h, const void* data, DWORD bytes)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD written = 0;
            if (!WriteFile(h, p, remaining, &written, nullptr))
            {
                return false;
            }
            p += written;
            remaining -= written;
        }
        return true;
    }

    bool ReadAll(HANDLE h, void* data, DWORD bytes)
    {
        uint8_t* p = static_cast<uint8_t*>(data);
        DWORD remaining = bytes;
        while (remaining > 0)
        {
            DWORD read = 0;
            if (!ReadFile(h, p, remaining, &read, nullptr))
            {
                return false;
            }
            if (read == 0)
            {
                return false;
            }
            p += read;
            remaining -= read;
        }
        return true;
    }

    bool HandleOneClient(HANDLE pipe, const std::function<AppIpc::Decision(const std::wstring&)>& onRequest)
    {
        uint32_t version = 0;
        uint32_t payloadBytes = 0;
        if (!ReadAll(pipe, &version, sizeof(version)) || !ReadAll(pipe, &payloadBytes, sizeof(payloadBytes)))
        {
            return false;
        }

        if (version != kProtocolVersion || payloadBytes == 0 || (payloadBytes % sizeof(wchar_t)) != 0)
        {
            return false;
        }

        std::vector<wchar_t> buf(payloadBytes / sizeof(wchar_t));
        if (!ReadAll(pipe, buf.data(), payloadBytes))
        {
            return false;
        }

        // Ensure null-termination (client sends a null-terminated string).
        if (buf.empty() || buf.back() != L'\0')
        {
            buf.push_back(L'\0');
        }

        const std::wstring path(buf.data());
        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        if (onRequest)
        {
            decision = onRequest(path);
        }

        const uint32_t resp = static_cast<uint32_t>(decision);
        return WriteAll(pipe, &resp, sizeof(resp));
    }
}

namespace AppIpc
{
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest)
    {
        FIC2_LOG_INFO("[IPC] StartServer: launching named-pipe server thread.");
        std::thread([onRequest]()
        {
            FIC2_LOG_DEBUG("[IPC] Server thread started. Pipe: {}", std::filesystem::path(kPipeName).string());
            for (;;)
            {
                HANDLE pipe = CreateNamedPipeW(
                    kPipeName,
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    PIPE_UNLIMITED_INSTANCES,
                    64 * 1024,
                    64 * 1024,
                    0,
                    nullptr);

                if (pipe == INVALID_HANDLE_VALUE)
                {
                    FIC2_LOG_ERROR("[IPC] Server: CreateNamedPipeW failed (err={}), retrying in 250ms.", GetLastError());
                    // Nothing we can do; retry after a short delay.
                    Sleep(250);
                    continue;
                }

                FIC2_LOG_DEBUG("[IPC] Server: pipe created, waiting for client connection...");
                const BOOL ok = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                if (!ok)
                {
                    FIC2_LOG_WARN("[IPC] Server: ConnectNamedPipe failed (err={}), discarding pipe.", GetLastError());
                    CloseHandle(pipe);
                    continue;
                }

                FIC2_LOG_DEBUG("[IPC] Server: client connected, handling request.");
                (void)HandleOneClient(pipe, onRequest);

                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                FIC2_LOG_DEBUG("[IPC] Server: client disconnected, looping for next connection.");
            }
        }).detach();
    }

    bool TrySendPath(const std::wstring& path, Decision& outDecision)
    {
        outDecision = Decision::Ignore;

        if (path.empty())
        {
            return false;
        }

        // Connect to the server pipe with retry logic.
        //
        // There are two distinct failure modes:
        //   ERROR_FILE_NOT_FOUND (2)  — pipe not yet created; server thread hasn't started.
        //                               WaitNamedPipeW returns immediately for this case,
        //                               so we must poll until the pipe appears.
        //   ERROR_PIPE_BUSY           — pipe exists but all server instances are occupied;
        //                               WaitNamedPipeW will block until one is free.
        //
        // Overall budget: 500 ms. The server typically creates the pipe within ~15 ms of
        // the mutex being visible, so polling at 5 ms intervals is more than sufficient.
        constexpr DWORD kTotalBudgetMs  = 500;
        constexpr DWORD kPollIntervalMs = 5;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kTotalBudgetMs);
        int retryCount = 0;

        HANDLE h = INVALID_HANDLE_VALUE;
        while (std::chrono::steady_clock::now() < deadline)
        {
            h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (h != INVALID_HANDLE_VALUE)
            {
                break;  // connected
            }

            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
            {
                // Pipe not yet created — server thread is still starting up.
                // Sleep briefly and retry.
                if (retryCount == 0)
                {
                    FIC2_LOG_INFO("[IPC] Client: pipe not yet created (server starting), polling every {}ms...",
                        kPollIntervalMs);
                }
                ++retryCount;
                Sleep(kPollIntervalMs);
                continue;
            }
            else if (err == ERROR_PIPE_BUSY)
            {
                // Pipe exists but busy — wait for a free slot then retry.
                FIC2_LOG_DEBUG("[IPC] Client: pipe busy, calling WaitNamedPipeW...");
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0 || !WaitNamedPipeW(kPipeName, static_cast<DWORD>(remaining)))
                {
                    FIC2_LOG_WARN("[IPC] Client: WaitNamedPipeW timed out (err={}).", GetLastError());
                    break;
                }
                continue;
            }
            else
            {
                FIC2_LOG_ERROR("[IPC] Client: CreateFileW (pipe connect) failed (err={}).", err);
                break;
            }
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

        if (retryCount > 0)
        {
            FIC2_LOG_INFO("[IPC] Client: connected after {} retries (~{}ms wait).",
                retryCount, retryCount * kPollIntervalMs);
        }
        FIC2_LOG_DEBUG("[IPC] Client: connected to server pipe. Sending path: {}",
            std::filesystem::path(path).string());

        const std::wstring payload = path + L'\0';
        const uint32_t version = kProtocolVersion;
        const uint32_t payloadBytes = static_cast<uint32_t>(payload.size() * sizeof(wchar_t));

        bool ok = true;
        ok = ok && WriteAll(h, &version, sizeof(version));
        ok = ok && WriteAll(h, &payloadBytes, sizeof(payloadBytes));
        ok = ok && WriteAll(h, payload.data(), payloadBytes);

        uint32_t resp = 0;
        ok = ok && ReadAll(h, &resp, sizeof(resp));

        CloseHandle(h);

        if (!ok)
        {
            FIC2_LOG_ERROR("[IPC] Client: pipe I/O failed during send/recv.");
            return false;
        }

        outDecision = static_cast<Decision>(resp);
        FIC2_LOG_INFO("[IPC] Client: server responded with decision={}.", static_cast<int>(outDecision));
        return true;
    }
}

