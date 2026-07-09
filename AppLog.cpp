#include "AppLog.h"

#include "framework.h"

#pragma warning(push)
#pragma warning(disable: 4996)
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifdef _DEBUG
#include <spdlog/sinks/msvc_sink.h>
#endif
#pragma warning(pop)

#include <filesystem>
#include <vector>
#include <mutex>
#include <algorithm>

namespace AppLog
{
    namespace
    {
        std::once_flag s_initFlag;

        // Remove old per-PID log files, keeping only the most recent 'keepCount'.
        void PruneOldLogs(const std::filesystem::path& logDir, int keepCount)
        {
            try
            {
                std::vector<std::filesystem::directory_entry> entries;
                for (const auto& e : std::filesystem::directory_iterator(logDir))
                {
                    const auto& p = e.path();
                    if (p.extension() == ".log" &&
                        p.stem().wstring().rfind(L"FICture2_", 0) == 0)
                    {
                        entries.push_back(e);
                    }
                }
                // Sort by last-write time, newest first.
                std::sort(entries.begin(), entries.end(),
                    [](const auto& a, const auto& b)
                    {
                        return a.last_write_time() > b.last_write_time();
                    });

                for (int i = keepCount; i < static_cast<int>(entries.size()); ++i)
                {
                    std::filesystem::remove(entries[i].path());
                }
            }
            catch (...) {}
        }
    }

    void Init(const std::wstring& executableDir)
    {
        std::wstring dir = executableDir;
        std::call_once(s_initFlag, [dir]()
        {
            try
            {
                const DWORD pid = GetCurrentProcessId();
                std::filesystem::path logDir = std::filesystem::path(dir);

                // Per-process file: FICture2_<PID>.log
                const std::string logName = "FICture2_" + std::to_string(pid) + ".log";
                std::filesystem::path logPath = logDir / logName;

                // Keep only the 5 most recent log files.
                PruneOldLogs(logDir, 5);

                std::vector<spdlog::sink_ptr> sinks;

                auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    logPath.string(), true /*truncate*/);
                fileSink->set_level(spdlog::level::info);
                sinks.push_back(fileSink);

#ifdef _DEBUG
                // OutputDebugString sink (visible in VS Output / DebugView).
                auto debugSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
                debugSink->set_level(spdlog::level::debug);
                sinks.push_back(debugSink);
#endif

                auto logger = std::make_shared<spdlog::logger>("fic2", sinks.begin(), sinks.end());

#ifdef _DEBUG
                // Debug: capture everything including debug/trace messages.
                logger->set_level(spdlog::level::debug);
                fileSink->set_level(spdlog::level::debug);
                logger->flush_on(spdlog::level::debug);
#else
                // Release: INFO and above only to minimize I/O overhead.
                logger->set_level(spdlog::level::info);
                logger->flush_on(spdlog::level::warn);
#endif

                logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%L] [%t] %v");

                spdlog::register_logger(logger);

#ifdef _DEBUG
                logger->info("=== FICture2 [PID {}] log started (debug build) ===", pid);
#else
                logger->info("=== FICture2 [PID {}] log started (release build) ===", pid);
#endif
                logger->info("Log file: {}", logPath.string());
                logger->flush();
            }
            catch (const spdlog::spdlog_ex& ex)
            {
                OutputDebugStringA("[FIC2] AppLog::Init failed: ");
                OutputDebugStringA(ex.what());
                OutputDebugStringA("\n");
            }
        });
    }

    void Shutdown()
    {
        if (auto logger = spdlog::get("fic2"))
        {
            logger->info("=== FICture2 log shutdown ===");
            logger->flush();
        }
        spdlog::shutdown();
    }
}
