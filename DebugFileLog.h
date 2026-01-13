#pragma once

#include <string>

namespace DebugFileLog
{
    // Appends a single line to a log file located next to the running FICture2.exe:
    //   <exe-dir>\FICture2_debug.log
    //
    // Best-effort: failures are silently ignored (e.g., no write permission).
    void WriteLine(const wchar_t* tag, const std::wstring& text);
    void WriteLine(const std::wstring& text);
}

