#include "TestHarness.h"

#include "AppSetup.h"
#include "ImageBrowserSessionPersistence.h"
#include "IniStore.h"
#include "RecentFiles.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// IniStore on temporary INI files only (never the user's FICture2.ini):
// Unicode output, layout kept on rewrite, ANSI migration, the batch API, the
// transaction lock (abandoned, timed out), failed commits, two-process races
// coordinated with named events, and the session and recent-files writers.

namespace
{
    // A unique folder under %TEMP%, removed with its contents on destruction.
    struct TempDir
    {
        std::filesystem::path dir {};

        TempDir()
        {
            static int counter = 0;
            wchar_t base[MAX_PATH] {};
            GetTempPathW(MAX_PATH, base);
            dir = std::filesystem::path(base) /
                (L"FICture2IniStoreTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter));
            std::filesystem::create_directories(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        std::wstring File(const wchar_t* name) const
        {
            return (dir / name).wstring();
        }

        std::vector<std::wstring> Names() const
        {
            std::vector<std::wstring> names;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                names.push_back(entry.path().filename().wstring());
            }
            return names;
        }
    };

    void WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        (void)WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    }

    std::vector<std::uint8_t> ReadOpenFile(HANDLE file)
    {
        std::vector<std::uint8_t> bytes;
        LARGE_INTEGER size {};
        GetFileSizeEx(file, &size);
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!bytes.empty())
        {
            (void)ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
        }
        bytes.resize(got);
        return bytes;
    }

    std::vector<std::uint8_t> ReadBytes(const std::wstring& path)
    {
        const HANDLE file = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        std::vector<std::uint8_t> bytes = ReadOpenFile(file);
        CloseHandle(file);
        return bytes;
    }

    // UTF-16LE with a BOM, the form IniStore writes.
    std::vector<std::uint8_t> Utf16Bytes(const std::wstring& text)
    {
        std::vector<std::uint8_t> bytes { 0xFF, 0xFE };
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        bytes.insert(bytes.end(), data, data + text.size() * sizeof(wchar_t));
        return bytes;
    }

    bool HasUtf16Bom(const std::vector<std::uint8_t>& bytes)
    {
        return bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE && (bytes.size() % 2) == 0;
    }

    std::wstring Utf16Text(const std::vector<std::uint8_t>& bytes)
    {
        if (!HasUtf16Bom(bytes))
        {
            return {};
        }
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    }

    // Text in the active ANSI code page, built at run time (never hard-coded
    // code-page bytes). Returns false if `text` is not representable.
    bool ToAnsi(const std::wstring& text, std::vector<std::uint8_t>& out)
    {
        BOOL usedDefault = FALSE;
        const int len = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, &usedDefault);
        if (len <= 0 || usedDefault)
        {
            return false;
        }
        out.resize(static_cast<std::size_t>(len));
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), reinterpret_cast<char*>(out.data()), len, nullptr, &usedDefault);
        if (usedDefault)
        {
            return false;
        }
        // It must also decode back unchanged.
        const int wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(out.data()), len, nullptr, 0);
        std::wstring back(static_cast<std::size_t>((std::max)(wlen, 0)), L'\0');
        MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(out.data()), len, back.data(), wlen);
        return back == text;
    }

    // A non-ASCII character the active code page can represent, or empty
    // (an ASCII-only code page).
    std::wstring AnsiNonAsciiSample()
    {
        const wchar_t* candidates[] = {
            L"\u00E9", L"\uD55C\uAE00", L"\u65E5\u672C", L"\u0416", L"\u00DF", L"\u03A9", L"\u05D0", L"\u0639", L"\u0E01", L"\u4E2D",
        };
        for (const wchar_t* candidate : candidates)
        {
            std::vector<std::uint8_t> bytes;
            if (ToAnsi(candidate, bytes))
            {
                return candidate;
            }
        }
        return {};
    }

    // Holds the INI's transaction lock on another thread until released.
    struct LockHolder
    {
        HANDLE held { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        HANDLE release { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        std::thread thread {};

        explicit LockHolder(const std::wstring& ini)
        {
            const std::wstring name = IniStore::LockName(ini);
            thread = std::thread([this, name]
            {
                const HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
                WaitForSingleObject(mutex, INFINITE);
                SetEvent(held);
                WaitForSingleObject(release, INFINITE);
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            });
            WaitForSingleObject(held, 10000);
        }

        ~LockHolder()
        {
            SetEvent(release);
            thread.join();
            CloseHandle(held);
            CloseHandle(release);
        }
    };

    std::string Hex(const std::wstring& text)
    {
        std::string out;
        char buf[8] {};
        for (const wchar_t c : text)
        {
            std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(static_cast<std::uint16_t>(c)));
            out += buf;
        }
        return out.empty() ? std::string("-") : out;
    }

    std::wstring Unhex(const char* hex)
    {
        std::wstring out;
        const std::size_t len = std::strlen(hex);
        for (std::size_t i = 0; i + 4 <= len; i += 4)
        {
            const std::string part(hex + i, 4);
            out += static_cast<wchar_t>(std::strtoul(part.c_str(), nullptr, 16));
        }
        return out;
    }

    std::wstring Widen(const std::string& ascii)
    {
        return std::wstring(ascii.begin(), ascii.end());
    }

    HANDLE NamedEvent(const std::string& prefix, const char* name)
    {
        return CreateEventW(nullptr, TRUE, FALSE, Widen("Local\\" + prefix + "-" + name).c_str());
    }

    std::string UniquePrefix()
    {
        static int counter = 0;
        return "FIC2IniStoreTest-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(++counter);
    }

    // `FICture2Tests --child <name> <args...>`, running concurrently.
    struct Child
    {
        PROCESS_INFORMATION process {};
        bool started { false };

        Child(const char* name, const std::vector<std::string>& args)
        {
            wchar_t exe[MAX_PATH] {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --child " + Widen(name);
            for (const std::string& arg : args)
            {
                commandLine += L" " + Widen(arg);
            }
            STARTUPINFOW startup { sizeof(startup) };
            started = CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
        }

        // The exit code, or -1 if the child did not finish in time.
        int Wait(DWORD timeoutMs = 30000)
        {
            if (!started)
            {
                return -1;
            }
            if (WaitForSingleObject(process.hProcess, timeoutMs) != WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
                return -1;
            }
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            return static_cast<int>(code);
        }

        ~Child()
        {
            if (started)
            {
                if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)
                {
                    TerminateProcess(process.hProcess, 1);
                    WaitForSingleObject(process.hProcess, 5000);
                }
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
    };

    constexpr int kFillerKeys = 3000;

    // A settings file large enough that a partly written copy would be seen
    // by the readers; [End] Sentinel=1 stays its last line (the racing writes
    // go to the existing [Parent] and [Child] sections).
    std::wstring RaceFixtureText(const std::wstring& nonAscii)
    {
        std::wstring text =
            L"[General]\r\nIniVersion=1\r\nInitialized=1\r\n"
            L"[Window]\r\nLeft=10\r\nTop=20\r\nRight=1210\r\nBottom=820\r\nShowCmd=1\r\n"
            L"[Viewer]\r\nShowNavItems=0\r\n"
            L"[Session]\r\nRecentFiles=C:\\Pictures\\" + nonAscii + L"\\a.png|C:\\b.png\r\n"
            L"[Parent]\r\nSeed=1\r\n[Child]\r\nSeed=1\r\n"
            L"[Filler]\r\n";
        for (int i = 0; i < kFillerKeys; ++i)
        {
            text += L"Key" + std::to_wstring(i) + L"=value-" + std::to_wstring(i) + L"\r\n";
        }
        text += L"[End]\r\nSentinel=1\r\n";
        return text;
    }

    // Decodes a raw INI as IniStore does (UTF-16LE BOM or the ANSI code page).
    std::wstring DecodeRaw(const std::vector<std::uint8_t>& bytes)
    {
        if (HasUtf16Bom(bytes))
        {
            return Utf16Text(bytes);
        }
        const int len = static_cast<int>(bytes.size());
        const int wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()), len, nullptr, 0);
        std::wstring out(static_cast<std::size_t>((std::max)(wlen, 0)), L'\0');
        MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()), len, out.data(), wlen);
        return out;
    }

    bool EndsWith(const std::wstring& text, const std::wstring& suffix)
    {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
}

// ';' is legal in file and folder names: a value keeps everything after '=',
// and only a line that starts with ';' or '#' is a comment.
FIC2_TEST(IniStore_ValueKeepsSemicolons)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"semicolons.ini");
    WriteBytes(ini, Utf16Bytes(
        L"; full-line comment\r\n"
        L"# hash comment\r\n"
        L"[Session]\r\n"
        L"RecentFiles=C:\\a;b\\x.png|C:\\c\\y.png|C:\\d;\\z.png\r\n"
        L"  ; Indented=comment\r\n"
        L"[Viewer0]\r\n"
        L"DisplayedFile=C:\\dir;semi\\img.png\r\n"
        L"CurrentFolder = C:\\dir;semi ; not a comment\r\n"));

    const IniStore store = IniStore::Load(ini);
    FIC2_REQUIRE(store.IsLoaded());
    FIC2_CHECK(store.GetString(L"Viewer0", L"DisplayedFile") == L"C:\\dir;semi\\img.png");
    FIC2_CHECK(store.GetString(L"Viewer0", L"CurrentFolder") == L"C:\\dir;semi ; not a comment");
    FIC2_CHECK(store.GetString(L"Session", L"; Indented", L"none") == L"none");

    const std::vector<std::wstring> recent = IniStore::SplitPipeList(store.GetString(L"Session", L"RecentFiles"));
    FIC2_REQUIRE(recent.size() == 3);
    FIC2_CHECK(recent[0] == L"C:\\a;b\\x.png");
    FIC2_CHECK(recent[1] == L"C:\\c\\y.png");
    FIC2_CHECK(recent[2] == L"C:\\d;\\z.png");
}

// Writes are UTF-16LE with a BOM, so characters outside any ANSI code page
// (emoji, Japanese, Thai) and ';' round-trip unchanged.
FIC2_TEST(IniStore_WritesUnicodeWithBom)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"new.ini");
    const std::wstring path = L"C:\\Pictures\\\U0001F600 \u65E5\u672C\u8A9E \u0E20\u0E32\u0E29\u0E32\u0E44\u0E17\u0E22;x\\img.png";

    FIC2_CHECK(IniStore::SetString(ini, L"Viewer0", L"DisplayedFile", path) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::SetInt(ini, L"Session", L"ViewerCount", 1) == IniStore::WriteStatus::Committed);

    const std::vector<std::uint8_t> bytes = ReadBytes(ini);
    FIC2_REQUIRE(HasUtf16Bom(bytes));
    FIC2_CHECK(Utf16Text(bytes) == L"[Viewer0]\r\nDisplayedFile=" + path + L"\r\n[Session]\r\nViewerCount=1\r\n");

    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetString(L"Viewer0", L"DisplayedFile") == path);
    FIC2_CHECK(store.GetInt(L"Session", L"ViewerCount") == 1);
}

// A rewrite keeps comments, blank lines and key order; a key set again keeps
// its line (the one reads use), a new key goes to the end of its section.
FIC2_TEST(IniStore_RewriteKeepsLayout)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"layout.ini");
    WriteBytes(ini, Utf16Bytes(
        L"; settings\r\n"
        L"[Window]\r\n"
        L"Left=1\r\n"
        L"Top=2\r\n"
        L"\r\n"
        L"# viewer\r\n"
        L"[Viewer]\r\n"
        L"ShowNavItems=1\r\n"));

    IniStore::Batch batch;
    batch.Put(L"window", L"TOP", L"20").PutInt(L"Window", L"Right", 30).Put(L"Session", L"ViewerCount", L"2");
    FIC2_CHECK(IniStore::Apply(ini, batch) == IniStore::WriteStatus::Committed);

    FIC2_CHECK(Utf16Text(ReadBytes(ini)) ==
        L"; settings\r\n"
        L"[Window]\r\n"
        L"Left=1\r\n"
        L"Top=20\r\n"
        L"Right=30\r\n"
        L"\r\n"
        L"# viewer\r\n"
        L"[Viewer]\r\n"
        L"ShowNavItems=1\r\n"
        L"[Session]\r\n"
        L"ViewerCount=2\r\n");
}

// An ANSI INI (the current release's format) keeps every setting when its
// first write migrates it to UTF-16LE. The fixture is built at run time in
// the active code page, with a non-ASCII character when it has one.
FIC2_TEST(IniStore_AnsiMigrationKeepsSettings)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"ansi.ini");
    const std::wstring nonAscii = AnsiNonAsciiSample();
    std::printf("  active code page %u, non-ASCII sample: %s\n", GetACP(), nonAscii.empty() ? "none" : "yes");
    const std::wstring folder = L"C:\\Pictures\\" + nonAscii + L"Folder";
    const std::wstring text =
        L"[General]\r\nIniVersion=1\r\nAskedAssociations=1\r\n"
        L"[Window]\r\nLeft=100\r\nTop=50\r\nRight=1300\r\nBottom=950\r\nShowCmd=3\r\nBackgroundColor=1,2,3\r\n"
        L"[Viewer]\r\nShowNavItems=0\r\n"
        L"[Session]\r\nViewerCount=1\r\nRecentFiles=" + folder + L"\\a.png|C:\\b;c.png\r\n"
        L"[Viewer0]\r\nCurrentFolder=" + folder + L"\r\n";
    std::vector<std::uint8_t> ansi;
    FIC2_REQUIRE(ToAnsi(text, ansi));
    WriteBytes(ini, ansi);

    // Read before migration: decoded with the ANSI code page.
    const IniStore before = IniStore::Load(ini);
    FIC2_CHECK(before.GetString(L"Viewer0", L"CurrentFolder") == folder);

    // The first write (a path no code page holds) migrates the file.
    const std::wstring emoji = L"C:\\\U0001F431\\cat.png";
    FIC2_CHECK(IniStore::SetString(ini, L"Viewer0", L"DisplayedFile", emoji) == IniStore::WriteStatus::Committed);

    const std::vector<std::uint8_t> bytes = ReadBytes(ini);
    FIC2_REQUIRE(HasUtf16Bom(bytes));
    const IniStore after = IniStore::Load(ini);
    FIC2_CHECK(after.GetInt(L"General", L"AskedAssociations") == 1);
    FIC2_CHECK(after.GetInt(L"Window", L"Left") == 100);
    FIC2_CHECK(after.GetInt(L"Window", L"Bottom") == 950);
    FIC2_CHECK(after.GetInt(L"Window", L"ShowCmd") == 3);
    FIC2_CHECK(after.GetString(L"Window", L"BackgroundColor") == L"1,2,3");
    FIC2_CHECK(after.GetInt(L"Viewer", L"ShowNavItems", 1) == 0);
    FIC2_CHECK(after.GetString(L"Session", L"RecentFiles") == folder + L"\\a.png|C:\\b;c.png");
    FIC2_CHECK(after.GetString(L"Viewer0", L"CurrentFolder") == folder);
    FIC2_CHECK(after.GetString(L"Viewer0", L"DisplayedFile") == emoji);
    // Everything else is the original text, line for line.
    FIC2_CHECK(Utf16Text(bytes) == text + L"DisplayedFile=" + emoji + L"\r\n");

    // An explicit migration of an ANSI file rewrites it the same way.
    const std::wstring ini2 = temp.File(L"ansi2.ini");
    WriteBytes(ini2, ansi);
    FIC2_CHECK(IniStore::MigrateToUnicode(ini2) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(Utf16Text(ReadBytes(ini2)) == text);
}

// Migrating a file that already has the BOM changes nothing, byte for byte,
// and so does a transaction that sets a key to its current value.
FIC2_TEST(IniStore_MigrationOnBomFileChangesNothing)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"bom.ini");
    // Hand-written, with LF line ends and spacing IniStore would not write.
    const std::vector<std::uint8_t> original = Utf16Bytes(L"[Window]\nLeft = 5\n; note\n[Session]\nRecentFiles=C:\\\u65E5\\a;b.png\n");
    WriteBytes(ini, original);
    FILETIME writeBefore {};
    {
        WIN32_FILE_ATTRIBUTE_DATA data {};
        GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &data);
        writeBefore = data.ftLastWriteTime;
    }

    FIC2_CHECK(IniStore::MigrateToUnicode(ini) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::MigrateToUnicode(ini) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::SetInt(ini, L"Window", L"Left", 5) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(ReadBytes(ini) == original);

    WIN32_FILE_ATTRIBUTE_DATA data {};
    GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &data);
    FIC2_CHECK(CompareFileTime(&writeBefore, &data.ftLastWriteTime) == 0);
    FIC2_CHECK(temp.Names().size() == 1);

    // A missing file is not created by a migration.
    const std::wstring missing = temp.File(L"missing.ini");
    FIC2_CHECK(IniStore::MigrateToUnicode(missing) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(!std::filesystem::exists(missing));
}

// A batch applies all of its keys in one transaction: a concurrent reader
// sees every key of a batch or none of it, never a mix of two batches.
FIC2_TEST(IniStore_BatchCommitsAllKeysAtOnce)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"batch.ini");
    constexpr int kKeys = 8;
    constexpr int kBatches = 150;

    std::atomic<bool> stop { false };
    std::atomic<int> reads { 0 };
    std::atomic<int> mixed { 0 };
    std::thread reader([&]
    {
        while (!stop.load())
        {
            const IniStore store = IniStore::Load(ini);
            if (!store.IsLoaded())
            {
                continue;
            }
            const std::wstring first = store.GetString(L"Batch", L"K0");
            for (int k = 1; k < kKeys; ++k)
            {
                if (store.GetString(L"Batch", L"K" + std::to_wstring(k)) != first)
                {
                    mixed.fetch_add(1);
                    break;
                }
            }
            reads.fetch_add(1);
        }
    });

    int committed = 0;
    for (int b = 1; b <= kBatches; ++b)
    {
        IniStore::Batch batch;
        for (int k = 0; k < kKeys; ++k)
        {
            batch.PutInt(L"Batch", L"K" + std::to_wstring(k), b);
        }
        committed += (IniStore::Apply(ini, batch) == IniStore::WriteStatus::Committed) ? 1 : 0;
    }
    stop.store(true);
    reader.join();

    std::printf("  %d batches committed, %d concurrent reads, %d mixed\n", committed, reads.load(), mixed.load());
    FIC2_CHECK(committed == kBatches);
    FIC2_CHECK(mixed.load() == 0);
    FIC2_CHECK(reads.load() > 0);
    const IniStore store = IniStore::Load(ini);
    for (int k = 0; k < kKeys; ++k)
    {
        FIC2_CHECK(store.GetInt(L"Batch", L"K" + std::to_wstring(k)) == kBatches);
    }

    // Transact reads the file as it is on disk inside the lock.
    FIC2_CHECK(IniStore::Transact(ini, [](IniStore& current)
    {
        current.PutInt(L"Batch", L"Sum", current.GetInt(L"Batch", L"K0") + current.GetInt(L"Batch", L"K1"));
    }) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::Load(ini).GetInt(L"Batch", L"Sum") == 2 * kBatches);
}

// A lock whose owner thread died (WAIT_ABANDONED) counts as acquired.
FIC2_TEST(IniStore_AbandonedLockByThreadIsAcquired)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"abandoned.ini");
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Before", L"1") == IniStore::WriteStatus::Committed);

    // Keep the mutex alive after its owner exits, so it is abandoned.
    const HANDLE keepAlive = CreateMutexW(nullptr, FALSE, IniStore::LockName(ini).c_str());
    FIC2_REQUIRE(keepAlive != nullptr);
    std::thread owner([&]
    {
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, IniStore::LockName(ini).c_str());
        WaitForSingleObject(mutex, INFINITE);
        CloseHandle(mutex);   // exits still owning it
    });
    owner.join();

    const auto start = std::chrono::steady_clock::now();
    FIC2_CHECK(IniStore::SetString(ini, L"A", L"After", L"2", 500) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(400));
    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetInt(L"A", L"Before") == 1);
    FIC2_CHECK(store.GetInt(L"A", L"After") == 2);
    CloseHandle(keepAlive);
}

// The same with a process that exits owning the lock.
FIC2_CHILD(IniStoreAbandonLockChild)
{
    if (argc < 2)
    {
        return 2;
    }
    const std::wstring ini = Unhex(argv[0]);
    const std::string prefix = argv[1];
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, IniStore::LockName(ini).c_str());
    if (mutex == nullptr || WaitForSingleObject(mutex, 10000) != WAIT_OBJECT_0)
    {
        return 3;
    }
    SetEvent(NamedEvent(prefix, "held"));
    return 0;   // exits still owning the mutex
}

FIC2_TEST(IniStore_AbandonedLockByProcessIsAcquired)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"abandoned-process.ini");
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Before", L"1") == IniStore::WriteStatus::Committed);

    const std::string prefix = UniquePrefix();
    const HANDLE held = NamedEvent(prefix, "held");
    const HANDLE keepAlive = CreateMutexW(nullptr, FALSE, IniStore::LockName(ini).c_str());
    FIC2_REQUIRE(keepAlive != nullptr);
    {
        Child child("IniStoreAbandonLockChild", { Hex(ini), prefix });
        FIC2_REQUIRE(WaitForSingleObject(held, 30000) == WAIT_OBJECT_0);
        FIC2_REQUIRE(child.Wait() == 0);
    }

    const auto start = std::chrono::steady_clock::now();
    FIC2_CHECK(IniStore::SetString(ini, L"A", L"After", L"2", 500) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(400));
    FIC2_CHECK(IniStore::Load(ini).GetInt(L"A", L"After") == 2);
    CloseHandle(keepAlive);
    CloseHandle(held);
}

// While another writer holds the lock, a transaction gives up after its
// timeout: nothing is written, the edit is not even run, and the status says
// LockTimeout. Readers still read (the file is always complete).
FIC2_TEST(IniStore_LockTimeoutSkipsWrite)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"timeout.ini");
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Value", L"old") == IniStore::WriteStatus::Committed);
    const std::vector<std::uint8_t> before = ReadBytes(ini);

    {
        const LockHolder holder(ini);
        bool editRan = false;
        const auto start = std::chrono::steady_clock::now();
        const IniStore::WriteStatus status = IniStore::Transact(ini, [&](IniStore& current)
        {
            editRan = true;
            current.Put(L"A", L"Value", L"new");
        }, 200);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        FIC2_CHECK(status == IniStore::WriteStatus::LockTimeout);
        FIC2_CHECK(!editRan);
        FIC2_CHECK(elapsed >= std::chrono::milliseconds(150));
        FIC2_CHECK(elapsed < std::chrono::milliseconds(1500));
        FIC2_CHECK(IniStore::StatusName(status) == std::string("LockTimeout"));
        FIC2_CHECK(ReadBytes(ini) == before);

        const IniStore reader = IniStore::Load(ini, 100);
        FIC2_CHECK(reader.GetString(L"A", L"Value") == L"old");
    }

    FIC2_CHECK(ReadBytes(ini) == before);
    FIC2_CHECK(IniStore::SetString(ini, L"A", L"Other", L"1") == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::Load(ini).GetString(L"A", L"Value") == L"old");
}

// A commit that cannot replace the file keeps the previous file, deletes its
// temporary file and reports CommitFailed; nothing is replayed later, and the
// next transaction starts from the file on disk.
FIC2_TEST(IniStore_FailedCommitKeepsPreviousFile)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"failed.ini");
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Value", L"old") == IniStore::WriteStatus::Committed);
    const std::vector<std::uint8_t> before = ReadBytes(ini);

    {
        // A reader that does not share delete access: the file can be read
        // but not replaced.
        const HANDLE blocker = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FIC2_REQUIRE(blocker != INVALID_HANDLE_VALUE);
        const auto start = std::chrono::steady_clock::now();
        const IniStore::WriteStatus status = IniStore::SetString(ini, L"A", L"Value", L"lost");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CloseHandle(blocker);
        FIC2_CHECK(status == IniStore::WriteStatus::CommitFailed);
        FIC2_CHECK(elapsed < std::chrono::milliseconds(IniStore::kCommitRetryMs + 1500));
    }
    FIC2_CHECK(ReadBytes(ini) == before);
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"failed.ini" });

    {
        // A file that cannot be read (here: locked byte range, while it could
        // still be replaced) is never overwritten.
        const HANDLE locked = CreateFileW(
            ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FIC2_REQUIRE(locked != INVALID_HANDLE_VALUE);
        OVERLAPPED range {};
        FIC2_REQUIRE(LockFileEx(locked, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &range));
        const IniStore::WriteStatus status = IniStore::SetString(ini, L"A", L"Value", L"lost-too");
        UnlockFileEx(locked, 0, MAXDWORD, MAXDWORD, &range);
        CloseHandle(locked);
        FIC2_CHECK(status == IniStore::WriteStatus::CommitFailed);
    }
    FIC2_CHECK(ReadBytes(ini) == before);
    FIC2_CHECK(temp.Names().size() == 1);

    // Another writer changes the file; the next transaction keeps that change
    // and the failed values never come back.
    WriteBytes(ini, Utf16Bytes(L"[A]\r\nValue=external\r\n"));
    FIC2_CHECK(IniStore::SetString(ini, L"B", L"Next", L"1") == IniStore::WriteStatus::Committed);
    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetString(L"A", L"Value") == L"external");
    FIC2_CHECK(store.GetInt(L"B", L"Next") == 1);
    FIC2_CHECK(Utf16Text(ReadBytes(ini)) == L"[A]\r\nValue=external\r\n[B]\r\nNext=1\r\n");
}

namespace
{
    // Injected swap calls for the destructive ReplaceFileW failures, as
    // documented: the replacement stays under its temporary name, and the
    // previous file is moved to the backup name
    // (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) or, without a backup name, deleted
    // (ERROR_UNABLE_TO_MOVE_REPLACEMENT). Moves are denied as configured.
    std::atomic<int> g_replaceCalls { 0 };
    std::atomic<int> g_moveCalls { 0 };
    std::atomic<bool> g_denyAllMoves { false };

    BOOL WINAPI ReplaceMovingTargetAside(LPCWSTR replaced, LPCWSTR, LPCWSTR backup, DWORD, LPVOID, LPVOID)
    {
        ++g_replaceCalls;
        if (backup == nullptr || *backup == L'\0')
        {
            (void)DeleteFileW(replaced);
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
            return FALSE;
        }
        if (!MoveFileExW(replaced, backup, MOVEFILE_REPLACE_EXISTING))
        {
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
            return FALSE;
        }
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return FALSE;
    }

    // Denies moving the temporary file in (and every move when
    // g_denyAllMoves is set); other moves are real.
    BOOL WINAPI MoveDenyingReplacement(LPCWSTR from, LPCWSTR to, DWORD flags)
    {
        ++g_moveCalls;
        const std::wstring source(from);
        if (g_denyAllMoves || (source.size() > 4 && source.compare(source.size() - 4, 4, L".tmp") == 0))
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        return MoveFileExW(from, to, flags);
    }

    struct InjectedSwapCalls
    {
        InjectedSwapCalls(bool denyAllMoves)
        {
            g_replaceCalls = 0;
            g_moveCalls = 0;
            g_denyAllMoves = denyAllMoves;
            IniStore::HooksForTesting().replace = &ReplaceMovingTargetAside;
            IniStore::HooksForTesting().move = &MoveDenyingReplacement;
        }

        ~InjectedSwapCalls()
        {
            IniStore::HooksForTesting() = {};
            g_denyAllMoves = false;
        }
    };
}

// A swap that fails after ReplaceFileW moved the previous file aside keeps the
// previous file: it is moved back; if even that is denied, it stays under the
// backup name, Load reads it, and the next transaction restores it before
// writing. The INI is never left missing with its contents lost.
FIC2_TEST(IniStore_FailedSwapAfterMovingTheFileAsideKeepsIt)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"swap.ini");
    const std::wstring backup = IniStore::BackupPath(ini);
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Value", L"old") == IniStore::WriteStatus::Committed);
    const std::vector<std::uint8_t> before = ReadBytes(ini);
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"swap.ini" });   // no backup after a commit

    {
        // The replacement cannot be moved in, but the previous file can be
        // moved back: it is in place again, unchanged.
        const InjectedSwapCalls inject(false);
        FIC2_CHECK(IniStore::SetString(ini, L"A", L"Value", L"lost") == IniStore::WriteStatus::CommitFailed);
        FIC2_CHECK(g_replaceCalls == 1);
        FIC2_CHECK(g_moveCalls > 1);
    }
    FIC2_CHECK(ReadBytes(ini) == before);
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"swap.ini" });

    {
        // Every move is denied: the previous file stays under the backup name
        // (never deleted), readers still get it, and a write is refused
        // rather than starting from an empty file.
        const InjectedSwapCalls inject(true);
        FIC2_CHECK(IniStore::SetString(ini, L"A", L"Value", L"lost") == IniStore::WriteStatus::CommitFailed);
        FIC2_CHECK(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES);
        FIC2_CHECK(ReadBytes(backup) == before);
        FIC2_CHECK(IniStore::Load(ini).GetString(L"A", L"Value") == L"old");
        FIC2_CHECK(IniStore::SetString(ini, L"B", L"Next", L"1") == IniStore::WriteStatus::CommitFailed);
        FIC2_CHECK(ReadBytes(backup) == before);
    }

    // The next transaction moves the backup back and writes on top of it.
    FIC2_CHECK(IniStore::SetString(ini, L"B", L"Next", L"2") == IniStore::WriteStatus::Committed);
    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetString(L"A", L"Value") == L"old");
    FIC2_CHECK(store.GetInt(L"B", L"Next") == 2);
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"swap.ini" });
}

namespace
{
    std::wstring g_handoffIni;

    // A transaction restoring the backup exactly between Load's open of the
    // missing INI and its open of the backup.
    void RestoreBackupBeforeBackupRead()
    {
        (void)MoveFileExW(IniStore::BackupPath(g_handoffIni).c_str(), g_handoffIni.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

// Load finds the previous file when a backup left by a failed swap is moved
// back between its two opens (INI missing, then backup missing).
FIC2_TEST(IniStore_LoadFollowsABackupMovedBackDuringTheRead)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"handoff.ini");
    WriteBytes(IniStore::BackupPath(ini), Utf16Bytes(L"[A]\r\nValue=old\r\n"));
    g_handoffIni = ini;
    IniStore::HooksForTesting().beforeBackupRead = &RestoreBackupBeforeBackupRead;
    const IniStore store = IniStore::Load(ini);
    IniStore::HooksForTesting() = {};

    FIC2_CHECK(store.IsLoaded());
    FIC2_CHECK(store.GetString(L"A", L"Value") == L"old");
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"handoff.ini" });
}

// Startup: an INI present only as the backup of an interrupted save is an
// existing configuration. It is not a first run, first-run setup does not
// replace it with defaults, and the next write restores it.
FIC2_TEST(IniStore_FirstRunSetupKeepsABackupOnlyConfiguration)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"FICture2.ini");
    const std::vector<std::uint8_t> saved = Utf16Bytes(
        L"[General]\r\nInitialized=1\r\n[Image]\r\nZoomStiffness=12.5\r\n"
        L"[Viewer]\r\nShowNavItems=0\r\n[Window]\r\nBackgroundColor=1,2,3\r\n");
    WriteBytes(IniStore::BackupPath(ini), saved);

    FIC2_CHECK(!FICture2App::IsFirstRun(ini));
    FICture2App::CreateIniIfMissing(ini, false);
    FIC2_CHECK(ReadBytes(IniStore::BackupPath(ini)) == saved);
    FIC2_CHECK(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES);

    FIC2_CHECK(IniStore::SetString(ini, L"Window", L"ShowCmd", L"1") == IniStore::WriteStatus::Committed);
    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetString(L"Window", L"BackgroundColor") == L"1,2,3");
    FIC2_CHECK(store.GetString(L"Image", L"ZoomStiffness") == L"12.5");
    FIC2_CHECK(store.GetInt(L"Viewer", L"ShowNavItems") == 0);
    FIC2_CHECK(temp.Names() == std::vector<std::wstring> { L"FICture2.ini" });

    // A real first run still gets the defaults.
    const std::wstring fresh = temp.File(L"fresh.ini");
    FIC2_CHECK(FICture2App::IsFirstRun(fresh));
    FICture2App::CreateIniIfMissing(fresh, false);
    FIC2_CHECK(!FICture2App::IsFirstRun(fresh));
    FIC2_CHECK(IniStore::Load(fresh).GetString(L"Window", L"BackgroundColor") == L"20,23,23");
}

// A sharing violation shorter than the retry budget does not fail the commit.
FIC2_TEST(IniStore_CommitRetriesBriefSharingViolation)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"retry.ini");
    FIC2_REQUIRE(IniStore::SetString(ini, L"A", L"Value", L"old") == IniStore::WriteStatus::Committed);

    const HANDLE blocker = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FIC2_REQUIRE(blocker != INVALID_HANDLE_VALUE);
    std::thread release([blocker]
    {
        Sleep(200);
        CloseHandle(blocker);
    });
    const IniStore::WriteStatus status = IniStore::SetString(ini, L"A", L"Value", L"new");
    release.join();
    FIC2_CHECK(status == IniStore::WriteStatus::Committed);
    FIC2_CHECK(IniStore::Load(ini).GetString(L"A", L"Value") == L"new");
    FIC2_CHECK(temp.Names().size() == 1);
}

// The lock is per file: the lowercased full path, however it is spelled.
FIC2_TEST(IniStore_LockNameFollowsFullPath)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"Name.ini");
    std::wstring upper = ini;
    for (wchar_t& c : upper)
    {
        c = static_cast<wchar_t>(std::towupper(c));
    }
    const std::wstring dotted = (temp.dir / L"." / L"name.INI").wstring();
    FIC2_CHECK(IniStore::LockName(ini) == IniStore::LockName(upper));
    FIC2_CHECK(IniStore::LockName(ini) == IniStore::LockName(dotted));
    FIC2_CHECK(IniStore::LockName(ini) != IniStore::LockName(temp.File(L"other.ini")));
    FIC2_CHECK(IniStore::LockName(ini).rfind(L"Local\\IniStore-", 0) == 0);
}

// Two-process races. Roles (FICture2Tests --child IniStoreRaceChild <ini> <prefix> <role> <key> <value>):
//   write    signal "ready", then write <key>=<value> in [Child] (waits for the lock)
//   hold     write <key>=<value> in [Child], signalling "in-lock" inside the
//            transaction and finishing it only after "continue"
//   read     until "stop": read the file unlocked (raw) and through Load, and
//            check each copy is complete (BOM or ANSI, ends with the sentinel)
// The exit code is 0 on success (read: no partial copy seen).
FIC2_CHILD(IniStoreRaceChild)
{
    if (argc < 5)
    {
        return 2;
    }
    const std::wstring ini = Unhex(argv[0]);
    const std::string prefix = argv[1];
    const std::string role = argv[2];
    const std::wstring key = Unhex(argv[3]);
    const std::wstring value = Unhex(argv[4]);

    if (role == "write")
    {
        SetEvent(NamedEvent(prefix, "ready"));
        return IniStore::SetString(ini, L"Child", key, value, 20000) == IniStore::WriteStatus::Committed ? 0 : 10;
    }
    if (role == "hold")
    {
        const HANDLE resume = NamedEvent(prefix, "continue");
        const IniStore::WriteStatus status = IniStore::Transact(ini, [&](IniStore& current)
        {
            SetEvent(NamedEvent(prefix, "in-lock"));
            WaitForSingleObject(resume, 20000);
            current.Put(L"Child", key, value);
        }, 20000);
        return status == IniStore::WriteStatus::Committed ? 0 : 11;
    }
    if (role == "read")
    {
        const HANDLE stop = NamedEvent(prefix, "stop");
        SetEvent(NamedEvent(prefix, "reading"));
        int reads = 0;
        int partial = 0;
        int unavailable = 0;
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
        {
            const HANDLE file = CreateFileW(
                ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                // ReplaceFileW briefly leaves the name missing or unopenable;
                // an unlocked reader retries (IniStore::Load does the same).
                ++unavailable;
                continue;
            }
            const std::wstring raw = DecodeRaw(ReadOpenFile(file));
            CloseHandle(file);
            ++reads;
            if (!EndsWith(raw, L"[End]\r\nSentinel=1\r\n") || raw.find(L"\r\nRight=1210\r\n") == std::wstring::npos)
            {
                ++partial;
            }
            // Every 8th pass also through IniStore (it parses the whole file).
            if ((reads % 8) == 0)
            {
                const IniStore store = IniStore::Load(ini, 0);
                if (store.GetString(L"End", L"Sentinel") != L"1" ||
                    store.GetString(L"Filler", L"Key" + std::to_wstring(kFillerKeys - 1)) != L"value-" + std::to_wstring(kFillerKeys - 1) ||
                    store.GetInt(L"Window", L"Right") != 1210)
                {
                    ++partial;
                }
            }
        }
        std::printf("  reader: %d reads, %d partial, %d briefly unavailable\n", reads, partial, unavailable);
        return (partial == 0 && reads > 0) ? 0 : 12;
    }
    return 2;
}

namespace
{
    // The parent writes [Parent] <key> while the child writes [Child] <key>.
    // parentFirst: the parent holds the lock (inside its transaction) until the
    // child has started its write; otherwise the child holds it until the
    // parent has started. A reader process checks every copy it sees.
    void RunTwoProcessRace(bool ansiFixture, bool parentFirst, bool parentMigratesOnly)
    {
        const TempDir temp;
        const std::wstring ini = temp.File(L"race.ini");
        const std::wstring nonAscii = AnsiNonAsciiSample();
        const std::wstring fixture = RaceFixtureText(nonAscii);
        if (ansiFixture)
        {
            std::vector<std::uint8_t> ansi;
            FIC2_REQUIRE(ToAnsi(fixture, ansi));
            WriteBytes(ini, ansi);
        }
        else
        {
            WriteBytes(ini, Utf16Bytes(fixture));
        }

        const std::wstring childValue = L"C:\\\U0001F600\\\u0E44\u0E17\u0E22;child.png";
        const std::wstring parentValue = L"C:\\\u65E5\u672C\\parent;\U0001F680.png";
        const std::string prefix = UniquePrefix();
        const HANDLE ready = NamedEvent(prefix, "ready");
        const HANDLE inLock = NamedEvent(prefix, "in-lock");
        const HANDLE resume = NamedEvent(prefix, "continue");
        const HANDLE stop = NamedEvent(prefix, "stop");
        const HANDLE reading = NamedEvent(prefix, "reading");

        Child reader("IniStoreRaceChild", { Hex(ini), prefix, "read", "-", "-" });
        FIC2_REQUIRE(WaitForSingleObject(reading, 30000) == WAIT_OBJECT_0);

        IniStore::WriteStatus parentStatus = IniStore::WriteStatus::CommitFailed;
        int childCode = -1;
        if (parentFirst)
        {
            // Barrier: the child starts its write while the parent is inside
            // its transaction; the child must wait for the lock and then see
            // the parent's (migrated) file.
            std::unique_ptr<Child> writer;
            parentStatus = IniStore::Transact(ini, [&](IniStore& current)
            {
                writer = std::make_unique<Child>("IniStoreRaceChild",
                    std::vector<std::string> { Hex(ini), prefix, "write", Hex(L"Value"), Hex(childValue) });
                FIC2_CHECK(WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0);
                Sleep(300);   // the child is now blocked on the lock
                if (!parentMigratesOnly)
                {
                    current.Put(L"Parent", L"Value", parentValue);
                }
            });
            childCode = writer ? writer->Wait() : -1;
        }
        else
        {
            Child writer("IniStoreRaceChild", { Hex(ini), prefix, "hold", Hex(L"Value"), Hex(childValue) });
            FIC2_REQUIRE(WaitForSingleObject(inLock, 30000) == WAIT_OBJECT_0);
            std::thread release([&]
            {
                Sleep(300);   // the parent is now blocked on the lock
                SetEvent(resume);
            });
            parentStatus = parentMigratesOnly
                ? IniStore::MigrateToUnicode(ini, 20000)
                : IniStore::SetString(ini, L"Parent", L"Value", parentValue, 20000);
            release.join();
            childCode = writer.Wait();
        }

        // Let the reader see the final file too.
        Sleep(100);
        SetEvent(stop);
        const int readerCode = reader.Wait();

        FIC2_CHECK(parentStatus == IniStore::WriteStatus::Committed);
        FIC2_CHECK(childCode == 0);
        FIC2_CHECK(readerCode == 0);

        const std::vector<std::uint8_t> bytes = ReadBytes(ini);
        FIC2_CHECK(HasUtf16Bom(bytes));
        const IniStore store = IniStore::Load(ini);
        FIC2_CHECK(store.GetString(L"Child", L"Value") == childValue);
        if (!parentMigratesOnly)
        {
            FIC2_CHECK(store.GetString(L"Parent", L"Value") == parentValue);
        }
        FIC2_CHECK(store.GetInt(L"Window", L"Right") == 1210);
        FIC2_CHECK(store.GetInt(L"Viewer", L"ShowNavItems", 1) == 0);
        FIC2_CHECK(store.GetString(L"Session", L"RecentFiles") == L"C:\\Pictures\\" + nonAscii + L"\\a.png|C:\\b.png");
        FIC2_CHECK(store.GetString(L"Filler", L"Key0") == L"value-0");
        FIC2_CHECK(store.GetString(L"End", L"Sentinel") == L"1");
        FIC2_CHECK(temp.Names().size() == 1);

        for (const HANDLE h : { ready, inLock, resume, stop, reading })
        {
            CloseHandle(h);
        }
    }
}

// Migration of an ANSI INI races a second settings writer in another process.
FIC2_TEST(IniStore_TwoProcessMigrationRacesSettingsWriter)
{
    // The parent's write migrates the file while the child waits to write.
    RunTwoProcessRace(true, true, false);
    // The parent only migrates, while the child holds the lock for its write.
    RunTwoProcessRace(true, false, true);
    // The parent's migration holds the lock while the child waits to write.
    RunTwoProcessRace(true, true, true);
}

// Two processes update independent keys of a Unicode INI at the same time.
FIC2_TEST(IniStore_TwoProcessOverlappingKeyUpdates)
{
    RunTwoProcessRace(false, true, false);
    RunTwoProcessRace(false, false, false);
}

namespace
{
    const std::wstring kEmojiPath = L"C:\\Pictures\\\U0001F600\U0001F308 \u65E5\u672C\u8A9E \u0E20\u0E32\u0E29\u0E32\u0E44\u0E17\u0E22\\photo.png";
    const std::wstring kSemicolonPath = L"C:\\Pictures\\a;b;c\\img;1.png";

    // An INI as the current release leaves it: ANSI, no BOM.
    void WriteReleaseAnsiIni(const std::wstring& ini)
    {
        const std::wstring text =
            L"[General]\r\nIniVersion=1\r\nInitialized=1\r\nAskedAssociations=1\r\nAssociationsEnabled=0\r\n"
            L"[Window]\r\nShowCmd=3\r\nBackgroundColor=30,30,30\r\nLeft=40\r\nTop=50\r\nRight=1240\r\nBottom=850\r\n"
            L"[Viewer]\r\nShowNavItems=0\r\n"
            L"[Session]\r\nViewerCount=1\r\nHorizontalSplitRatios=\r\nRecentFiles=C:\\old.png\r\n"
            L"[Viewer0]\r\nDisplayedFile=C:\\old.png\r\nCurrentFolder=C:\\\r\n";
        std::vector<std::uint8_t> ansi;
        ToAnsi(text, ansi);
        WriteBytes(ini, ansi);
    }
}

// Session paths with emoji, Japanese, Thai and ';' round-trip through save
// and restore unchanged, starting from a release (ANSI) INI whose other
// settings are kept.
FIC2_TEST(IniStore_SessionRoundTripsUnicodeAndSemicolonPaths)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"session.ini");
    WriteReleaseAnsiIni(ini);

    ImageBrowserSessionPersistence::SavePayload payload {};
    payload.viewerCount = 3;
    payload.hasThumbStripHeight = true;
    payload.thumbStripHeight = 180.0f;
    payload.horizontalSplitRatios = { 0.25f, 0.5f };
    payload.viewers = {
        { kEmojiPath, L"C:\\Pictures\\\U0001F600\U0001F308 \u65E5\u672C\u8A9E \u0E20\u0E32\u0E29\u0E32\u0E44\u0E17\u0E22" },
        { kSemicolonPath, L"C:\\Pictures\\a;b;c" },
        { L"", L"D:\\\u0E44\u0E17\u0E22;\u65E5" },
    };
    FIC2_CHECK(ImageBrowserSessionPersistence::SaveToIni(ini, payload) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(HasUtf16Bom(ReadBytes(ini)));

    ImageBrowserSessionPersistence::RestorePayload restored {};
    FIC2_REQUIRE(ImageBrowserSessionPersistence::TryRestoreFromIni(ini, restored));
    FIC2_CHECK(restored.viewerCount == 3);
    FIC2_REQUIRE(restored.viewers.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
    {
        FIC2_CHECK(restored.viewers[i].filePath == payload.viewers[i].displayedFile);
        FIC2_CHECK(restored.viewers[i].folderPath == payload.viewers[i].currentFolder);
    }
    FIC2_CHECK(restored.hasThumbStripHeight && restored.thumbStripHeight == 180.0f);
    FIC2_CHECK(restored.horizontalSplitRatios.size() == 2);

    const IniStore store = IniStore::Load(ini);
    FIC2_CHECK(store.GetInt(L"Window", L"ShowCmd") == 3);
    FIC2_CHECK(store.GetInt(L"Window", L"Right") == 1240);
    FIC2_CHECK(store.GetString(L"Window", L"BackgroundColor") == L"30,30,30");
    FIC2_CHECK(store.GetInt(L"Viewer", L"ShowNavItems", 1) == 0);
    FIC2_CHECK(store.GetInt(L"General", L"AskedAssociations") == 1);
    FIC2_CHECK(store.GetString(L"Session", L"RecentFiles") == L"C:\\old.png");
}

// A session save is one transaction: a concurrent reader never sees the keys
// of two different saves mixed.
FIC2_TEST(IniStore_SessionSaveIsOneTransaction)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"session-atomic.ini");
    WriteReleaseAnsiIni(ini);

    auto makePayload = [](int generation)
    {
        ImageBrowserSessionPersistence::SavePayload payload {};
        payload.viewerCount = 2;
        payload.horizontalSplitRatios = { 0.5f };
        const std::wstring tag = std::to_wstring(generation);
        payload.viewers = {
            { L"C:\\" + tag + L"\\a.png", L"C:\\" + tag },
            { L"D:\\" + tag + L"\\b.png", L"D:\\" + tag },
        };
        return payload;
    };

    constexpr int kSaves = 100;
    std::atomic<bool> stop { false };
    std::atomic<int> reads { 0 };
    std::atomic<int> mixed { 0 };
    std::thread reader([&]
    {
        while (!stop.load())
        {
            ImageBrowserSessionPersistence::RestorePayload restored {};
            if (!ImageBrowserSessionPersistence::TryRestoreFromIni(ini, restored) || restored.viewers.size() != 2)
            {
                continue;
            }
            const std::wstring tag = restored.viewers[0].folderPath.substr(3);
            if (restored.viewers[0].filePath != L"C:\\" + tag + L"\\a.png" ||
                restored.viewers[1].folderPath != L"D:\\" + tag ||
                restored.viewers[1].filePath != L"D:\\" + tag + L"\\b.png")
            {
                mixed.fetch_add(1);
            }
            reads.fetch_add(1);
        }
    });

    int committed = 0;
    for (int g = 1; g <= kSaves; ++g)
    {
        committed += (ImageBrowserSessionPersistence::SaveToIni(ini, makePayload(g)) == IniStore::WriteStatus::Committed) ? 1 : 0;
    }
    stop.store(true);
    reader.join();

    std::printf("  %d saves committed, %d concurrent restores, %d mixed\n", committed, reads.load(), mixed.load());
    FIC2_CHECK(committed == kSaves);
    FIC2_CHECK(mixed.load() == 0);
    FIC2_CHECK(reads.load() > 0);
}

// Recent files: entries with emoji, Japanese, Thai and ';' keep their exact
// paths, and a list in which an entry contains ';' keeps all of its entries.
FIC2_TEST(IniStore_RecentFilesKeepUnicodeAndSemicolonEntries)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    WriteReleaseAnsiIni(ini);

    const std::vector<std::wstring> added = {
        L"C:\\first.png",
        kSemicolonPath,
        kEmojiPath,
        L"C:\\x;y\\last;.png",
    };
    for (const std::wstring& path : added)
    {
        FIC2_CHECK(RecentFiles::Add(ini, path) == IniStore::WriteStatus::Committed);
    }

    const std::vector<std::wstring> recent = RecentFiles::Load(ini);
    FIC2_REQUIRE(recent.size() == added.size() + 1);   // + C:\old.png from the release INI
    for (std::size_t i = 0; i < added.size(); ++i)
    {
        FIC2_CHECK(recent[i] == added[added.size() - 1 - i]);
    }
    FIC2_CHECK(recent.back() == L"C:\\old.png");
    FIC2_CHECK(HasUtf16Bom(ReadBytes(ini)));

    // Re-adding an entry moves it to the front without duplicating it.
    FIC2_CHECK(RecentFiles::Add(ini, kSemicolonPath) == IniStore::WriteStatus::Committed);
    const std::vector<std::wstring> again = RecentFiles::Load(ini);
    FIC2_CHECK(again.size() == recent.size());
    FIC2_CHECK(!again.empty() && again.front() == kSemicolonPath);

    FIC2_CHECK(RecentFiles::Clear(ini) == IniStore::WriteStatus::Committed);
    FIC2_CHECK(RecentFiles::Load(ini).empty());
    FIC2_CHECK(IniStore::Load(ini).GetInt(L"Window", L"ShowCmd") == 3);
}
