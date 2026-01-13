#include "DebugFileLog.h"

#include <windows.h>

#include <mutex>

namespace
{
    std::once_flag g_initOnce;
    std::wstring g_logPath;
    std::mutex g_writeMutex;

    void EnsureLogPathInitialized()
    {
        std::call_once(g_initOnce, []()
        {
            wchar_t exePath[MAX_PATH] {};
            const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return;
            }

            std::wstring full(exePath);
            size_t slash = full.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return;
            }

            const std::wstring dir = full.substr(0, slash);
            g_logPath = dir + L"\\FICture2_debug.log";
        });
    }

    void AppendLine(const std::wstring& line)
    {
        EnsureLogPathInitialized();
        if (g_logPath.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(g_writeMutex);

        HANDLE h = CreateFileW(
            g_logPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }

        LARGE_INTEGER size {};
        if (GetFileSizeEx(h, &size) && size.QuadPart == 0)
        {
            // Write UTF-16LE BOM so Notepad opens the file correctly.
            const wchar_t bom = 0xFEFF;
            DWORD bomWritten = 0;
            (void)WriteFile(h, &bom, sizeof(bom), &bomWritten, nullptr);
        }

        std::wstring out = line;
        out += L"\r\n";

        DWORD bytesWritten = 0;
        (void)WriteFile(h, out.data(), static_cast<DWORD>(out.size() * sizeof(wchar_t)), &bytesWritten, nullptr);
        CloseHandle(h);
    }
}

namespace DebugFileLog
{
    void WriteLine(const wchar_t* tag, const std::wstring& text)
    {
        SYSTEMTIME st {};
        GetLocalTime(&st);

        wchar_t buf[4096] {};
        const wchar_t* safeTag = (tag != nullptr && tag[0] != 0) ? tag : L"LOG";
        const wchar_t* safeText = (!text.empty()) ? text.c_str() : L"";

        swprintf_s(
            buf,
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds,
            safeTag,
            safeText);

        AppendLine(buf);
    }

    void WriteLine(const std::wstring& text)
    {
        WriteLine(L"LOG", text);
    }
}

