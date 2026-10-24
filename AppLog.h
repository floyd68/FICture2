#pragma once

// AppLog: optional file logging via spdlog.
//
// Logging is controlled by command-line switches:
//   Debug   builds: ON  by default; pass -logoff to disable.
//   Release builds: OFF by default; pass -logon  to enable.
//
// Usage:
//   - Call AppLog::Init(executableDir) only when logging is requested.
//     If Init is never called, every FIC2_LOG_* macro is a silent no-op.
//   - Call AppLog::Shutdown() before process exit (safe even if Init was skipped).
//   - Use the FIC2_LOG_* macros for structured logging.
//   - Use FIC2_TIMER_START / FIC2_LOG_STEP for startup timing.
//
// Debug builds:   file sink + OutputDebugString sink, DEBUG level.
// Release builds: file sink only, INFO level (DEBUG/TRACE macros are no-ops).

#include <filesystem>
#include <string>
#include <string_view>

namespace AppLog
{
    // Initialize spdlog: file sink (FICture2_<PID>.log in executableDir).
    // In Debug builds an OutputDebugString sink is added as well.
    // Safe to call multiple times; subsequent calls are no-ops.
    // Never throws: if the logger cannot be created, logging stays disabled.
    void Init(const std::wstring& executableDir);

    // Flush and shut down the logger.
    void Shutdown();

    // Non-throwing UTF-16 -> UTF-8 conversion for log arguments. Use this for
    // paths and other wide strings, never std::filesystem::path::string(),
    // which uses the active code page and throws for characters outside it.
    // Unpaired surrogates become U+FFFD.
    std::string ToUtf8(std::wstring_view text) noexcept;
    std::string ToUtf8(const std::filesystem::path& path) noexcept;
    // Exact-match overloads: std::wstring and wide literals convert to both
    // std::wstring_view and std::filesystem::path, which would be ambiguous.
    inline std::string ToUtf8(const std::wstring& text) noexcept { return ToUtf8(std::wstring_view(text)); }
    inline std::string ToUtf8(const wchar_t* text) noexcept
    {
        return text ? ToUtf8(std::wstring_view(text)) : std::string();
    }
}

#pragma warning(push)
#pragma warning(disable: 4996)  // 'localtime': may be unsafe (spdlog internal)
#include <spdlog/spdlog.h>
#pragma warning(pop)

// ---------------------------------------------------------------------------
// Logging macros — forward to the "fic2" named logger.
// Arguments are evaluated only when the logger exists (AppLog::Init succeeded).
// In Release builds DEBUG and TRACE macros are stripped to avoid overhead.
// ---------------------------------------------------------------------------
#define FIC2_LOG_INFO(...)   do { if (auto _l = spdlog::get("fic2")) _l->info(__VA_ARGS__);  } while (0)
#define FIC2_LOG_WARN(...)   do { if (auto _l = spdlog::get("fic2")) _l->warn(__VA_ARGS__);  } while (0)
#define FIC2_LOG_ERROR(...)  do { if (auto _l = spdlog::get("fic2")) _l->error(__VA_ARGS__); } while (0)

#ifdef _DEBUG
#define FIC2_LOG_TRACE(...)  do { if (auto _l = spdlog::get("fic2")) _l->trace(__VA_ARGS__); } while (0)
#define FIC2_LOG_DEBUG(...)  do { if (auto _l = spdlog::get("fic2")) _l->debug(__VA_ARGS__); } while (0)
#else
#define FIC2_LOG_TRACE(...)  do {} while (0)
#define FIC2_LOG_DEBUG(...)  do {} while (0)
#endif

// ---------------------------------------------------------------------------
// Startup timing helpers.
//
// Usage:
//   FIC2_TIMER_START(t0);           // capture baseline
//   ... do work ...
//   FIC2_LOG_STEP(t0, "step name"); // log elapsed ms and reset t0
// ---------------------------------------------------------------------------
#include <chrono>
#define FIC2_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define FIC2_LOG_STEP(var, label) \
    do { \
        auto _now = std::chrono::steady_clock::now(); \
        auto _ms  = std::chrono::duration_cast<std::chrono::milliseconds>(_now - (var)).count(); \
        FIC2_LOG_INFO("[STARTUP] {:>6}ms  {}", _ms, label); \
        (var) = _now; \
    } while (0)

// Milliseconds elapsed since a FIC2_TIMER_START baseline (does not reset it).
// Useful for threshold-based stall logging:
//   FIC2_TIMER_START(t0);
//   ... do work ...
//   if (FIC2_ELAPSED_MS(t0) > 50) { FIC2_LOG_INFO(...); }
#define FIC2_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())
