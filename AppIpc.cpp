#include "AppIpc.h"

#include <windows.h>

#include <cstdint>
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
        std::thread([onRequest]()
        {
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
                    // Nothing we can do; retry after a short delay.
                    Sleep(250);
                    continue;
                }

                const BOOL ok = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                if (!ok)
                {
                    CloseHandle(pipe);
                    continue;
                }

                (void)HandleOneClient(pipe, onRequest);

                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
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

        // If the server isn't up yet, don't block long; we want the new instance to continue.
        if (!WaitNamedPipeW(kPipeName, 150))
        {
            return false;
        }

        HANDLE h = CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }

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
            return false;
        }

        outDecision = static_cast<Decision>(resp);
        return true;
    }
}

