#include "TestHarness.h"

#include "AppLog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// Covers application logging with Unicode paths: the non-throwing UTF-8
// conversion, argument laziness without a logger, AppLog::Init in a folder
// outside the active code page, and Init failure degrading to no logging.
// AppLog::Init runs once per process, so each Init scenario is its own CTest
// entry (see tests/CMakeLists.txt).

namespace
{
    // Emoji (surrogate pair), Japanese, and Thai: outside every single-byte or
    // East Asian ANSI code page at once.
    const std::wstring kNonAcpStem = L"\U0001F600_\u30C6\u30B9\u30C8_\u0E44\u0E17\u0E22";
    const std::string kNonAcpStemUtf8 =
        "\xF0\x9F\x98\x80_\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88_\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2";

    int g_evaluations = 0;

    int CountedArgument()
    {
        ++g_evaluations;
        return 42;
    }

    bool IsValidUtf8(const std::string& text)
    {
        return text.empty() ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
    }

    std::filesystem::path TempTestDir(const std::wstring& stem)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        return std::filesystem::path(tempPath) /
            (L"FICture2Tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + stem);
    }

    struct ScopedDir
    {
        std::filesystem::path path;
        explicit ScopedDir(std::filesystem::path dir)
            : path(std::move(dir))
        {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
        }
        ~ScopedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::filesystem::path LogFileIn(const std::filesystem::path& dir)
    {
        return dir / (L"FICture2_" + std::to_wstring(GetCurrentProcessId()) + L".log");
    }
}

FIC2_TEST(AppLogText_ToUtf8ConvertsNonAcpText)
{
    FIC2_CHECK(AppLog::ToUtf8(kNonAcpStem) == kNonAcpStemUtf8);
    FIC2_CHECK(AppLog::ToUtf8(kNonAcpStem.c_str()) == kNonAcpStemUtf8);
    FIC2_CHECK(AppLog::ToUtf8(std::wstring_view(kNonAcpStem)) == kNonAcpStemUtf8);
    FIC2_CHECK(AppLog::ToUtf8(std::filesystem::path(L"C:\\") / kNonAcpStem) == "C:\\" + kNonAcpStemUtf8);
    FIC2_CHECK(AppLog::ToUtf8(std::wstring()).empty());
    FIC2_CHECK(AppLog::ToUtf8(static_cast<const wchar_t*>(nullptr)).empty());

    // An unpaired surrogate does not throw; it becomes U+FFFD.
    const std::wstring lone = L"a\xD800" L"b";
    FIC2_CHECK(AppLog::ToUtf8(lone) == "a\xEF\xBF\xBD" "b");
}

FIC2_TEST(AppLogLazy_ArgumentsNotEvaluatedWithoutLogger)
{
    // AppLog::Init is never called in this process.
    g_evaluations = 0;
    FIC2_LOG_INFO("value {}", CountedArgument());
    FIC2_LOG_WARN("value {}", CountedArgument());
    FIC2_LOG_ERROR("value {} {}", CountedArgument(), AppLog::ToUtf8(kNonAcpStem));
    FIC2_LOG_DEBUG("value {}", CountedArgument());
    FIC2_CHECK(g_evaluations == 0);
}

FIC2_TEST(AppLogInitUnicode_LogsUtf8InNonAcpFolder)
{
    ScopedDir dir(TempTestDir(kNonAcpStem));
    FIC2_REQUIRE(std::filesystem::is_directory(dir.path));

    AppLog::Init(dir.path.wstring());
    FIC2_REQUIRE(spdlog::get("fic2") != nullptr);

    g_evaluations = 0;
    FIC2_LOG_INFO("value {} path {}", CountedArgument(), AppLog::ToUtf8(dir.path / L"image.dds"));
    FIC2_CHECK(g_evaluations == 1);
    AppLog::Shutdown();

    std::ifstream in(LogFileIn(dir.path), std::ios::binary);
    FIC2_REQUIRE(in.is_open());
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    FIC2_CHECK(IsValidUtf8(contents));
    // "Log file:" line written by Init, and the message above.
    FIC2_CHECK(contents.find("Log file: ") != std::string::npos);
    FIC2_CHECK(contents.find(AppLog::ToUtf8(LogFileIn(dir.path))) != std::string::npos);
    FIC2_CHECK(contents.find("value 42 path " + AppLog::ToUtf8(dir.path / L"image.dds")) != std::string::npos);
}

FIC2_TEST(AppLogInitFailure_DegradesToDisabledLogging)
{
    // The log folder is a regular file, so the log file cannot be created.
    ScopedDir dir(TempTestDir(L"init_failure_" + kNonAcpStem));
    const std::filesystem::path notADir = dir.path / L"not_a_folder";
    {
        std::ofstream file(notADir);
    }
    FIC2_REQUIRE(std::filesystem::is_regular_file(notADir));

    AppLog::Init(notADir.wstring());
    FIC2_CHECK(spdlog::get("fic2") == nullptr);

    g_evaluations = 0;
    FIC2_LOG_ERROR("value {}", CountedArgument());
    FIC2_CHECK(g_evaluations == 0);
    AppLog::Shutdown();
}
