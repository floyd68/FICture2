#include "TestHarness.h"

#include "AppLog.h"
#include "IniStore.h"
#include "RecentFiles.h"

#include <spdlog/sinks/ostream_sink.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// RecentFilesStore on temporary INI files only (never the user's FICture2.ini),
// with fake clocks: no INI I/O on Add, one transaction per flush, the merge
// with a fresh read, the cross-instance Clear epoch, failed flushes and Clears
// kept for a retry, the menu merge, and the before-destroy flush.

namespace
{
    constexpr DWORD kLockTimeoutMs = 200;

    struct TempDir
    {
        std::filesystem::path dir {};

        TempDir()
        {
            static int counter = 0;
            wchar_t base[MAX_PATH] {};
            GetTempPathW(MAX_PATH, base);
            dir = std::filesystem::path(base) /
                (L"FICture2RecentFilesTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
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
    };

    // The file's bytes and last write time: equal snapshots mean untouched.
    struct FileSnapshot
    {
        std::vector<std::uint8_t> bytes {};
        std::uint64_t writeTime = 0;

        bool operator==(const FileSnapshot&) const = default;
    };

    FileSnapshot Snapshot(const std::wstring& path)
    {
        FileSnapshot snapshot;
        WIN32_FILE_ATTRIBUTE_DATA data {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            snapshot.writeTime = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                data.ftLastWriteTime.dwLowDateTime;
        }
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            std::uint8_t buffer[4096];
            DWORD got = 0;
            while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                snapshot.bytes.insert(snapshot.bytes.end(), buffer, buffer + got);
            }
            CloseHandle(file);
        }
        return snapshot;
    }

    std::vector<std::wstring> PersistedList(const std::wstring& ini)
    {
        return IniStore::SplitPipeList(IniStore::Load(ini).GetString(L"Session", L"RecentFiles"));
    }

    std::uint64_t PersistedClearedAt(const std::wstring& ini)
    {
        return RecentFilesStore::ParseClearedAt(IniStore::Load(ini).GetString(L"Session", L"RecentFilesClearedAt"));
    }

    void SeedIni(const std::wstring& ini, const std::wstring& list, const std::wstring& clearedAt = L"")
    {
        IniStore::Batch batch;
        batch.Put(L"Window", L"ShowCmd", L"1");
        batch.Put(L"Session", L"RecentFiles", list);
        if (!clearedAt.empty())
        {
            batch.Put(L"Session", L"RecentFilesClearedAt", clearedAt);
        }
        FIC2_REQUIRE(IniStore::Apply(ini, batch) == IniStore::WriteStatus::Committed);
    }

    // A store whose clock reads `now`.
    std::unique_ptr<RecentFilesStore> MakeStore(const std::wstring& ini, const std::uint64_t& now)
    {
        return std::make_unique<RecentFilesStore>(ini, [&now]() { return now; }, kLockTimeoutMs);
    }

    // Holds the INI's transaction lock on another thread (the named mutex is
    // recursive for its owner, so the test thread cannot hold it itself).
    class LockHolder
    {
    public:
        explicit LockHolder(const std::wstring& ini)
        {
            m_acquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            m_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            const std::wstring name = IniStore::LockName(ini);
            m_thread = std::thread([this, name]()
            {
                const HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
                (void)WaitForSingleObject(mutex, INFINITE);
                SetEvent(m_acquired);
                (void)WaitForSingleObject(m_release, INFINITE);
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            });
            (void)WaitForSingleObject(m_acquired, INFINITE);
        }

        ~LockHolder()
        {
            Release();
            CloseHandle(m_acquired);
            CloseHandle(m_release);
        }

        void Release()
        {
            if (m_thread.joinable())
            {
                SetEvent(m_release);
                m_thread.join();
            }
        }

    private:
        HANDLE m_acquired = nullptr;
        HANDLE m_release = nullptr;
        std::thread m_thread {};
    };

    BOOL WINAPI FailReplace(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    BOOL WINAPI FailMove(LPCWSTR, LPCWSTR, DWORD)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Every commit fails (CommitFailed) while this lives; the INI is kept.
    struct FailingCommits
    {
        FailingCommits()
        {
            IniStore::HooksForTesting().replace = &FailReplace;
            IniStore::HooksForTesting().move = &FailMove;
        }

        ~FailingCommits()
        {
            IniStore::HooksForTesting() = {};
        }
    };

    // Captures what the app logs through the "fic2" logger.
    class LogCapture
    {
    public:
        LogCapture()
        {
            m_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_text);
            m_sink->set_pattern("%v");
            m_logger = spdlog::get("fic2");
            if (m_logger)
            {
                m_logger->sinks().push_back(m_sink);
            }
            else
            {
                m_logger = std::make_shared<spdlog::logger>("fic2", m_sink);
                spdlog::register_logger(m_logger);
                m_registered = true;
            }
        }

        ~LogCapture()
        {
            if (m_registered)
            {
                spdlog::drop("fic2");
            }
            else
            {
                auto& sinks = m_logger->sinks();
                sinks.erase(std::remove(sinks.begin(), sinks.end(), m_sink), sinks.end());
            }
        }

        bool Contains(const char* text)
        {
            m_logger->flush();
            return m_text.str().find(text) != std::string::npos;
        }

    private:
        std::ostringstream m_text {};
        std::shared_ptr<spdlog::sinks::ostream_sink_mt> m_sink {};
        std::shared_ptr<spdlog::logger> m_logger {};
        bool m_registered = false;
    };

    enum class FailureKind
    {
        LockTimeout,
        CommitFailed,
    };

    const char* FailureText(FailureKind kind)
    {
        return kind == FailureKind::LockTimeout ? "RecentFiles not saved: LockTimeout" : "RecentFiles not saved: CommitFailed";
    }

    IniStore::WriteStatus FailureStatus(FailureKind kind)
    {
        return kind == FailureKind::LockTimeout ? IniStore::WriteStatus::LockTimeout : IniStore::WriteStatus::CommitFailed;
    }

    // Runs `write` while the INI's commits fail the given way.
    template <typename Fn>
    IniStore::WriteStatus WithFailure(FailureKind kind, const std::wstring& ini, Fn&& write)
    {
        if (kind == FailureKind::LockTimeout)
        {
            const LockHolder lock(ini);
            return write();
        }
        const FailingCommits failing;
        return write();
    }

    std::wstring PathN(int i)
    {
        return L"C:\\pics\\img" + std::to_wstring(i) + L".png";
    }
}

// Add only records in memory: N Adds read and write nothing (they do not even
// wait for the INI's lock, which another thread holds) and only re-arm the
// debounce timer; one Flush is one transaction and keeps the newest
// kMaxEntries in local order.
FIC2_TEST(RecentFiles_AddDoesNoIniIoAndFlushIsOneTransaction)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\old.png");
    const FileSnapshot before = Snapshot(ini);

    std::uint64_t now = 1000;
    auto store = MakeStore(ini, now);
    int arms = 0;
    int disarms = 0;
    store->SetFlushScheduler([&](bool arm) { ++(arm ? arms : disarms); });

    constexpr int kAdds = 200;
    {
        const LockHolder lock(ini);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kAdds; ++i)
        {
            ++now;
            store->Add(PathN(i));
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        FIC2_CHECK(elapsed < std::chrono::milliseconds(kLockTimeoutMs));
    }
    FIC2_CHECK(store->ReadCountForTesting() == 0);
    FIC2_CHECK(store->TransactionCountForTesting() == 0);
    FIC2_CHECK(Snapshot(ini) == before);
    FIC2_CHECK(arms == kAdds);
    FIC2_CHECK(disarms == 0);
    FIC2_CHECK(store->HasPendingChanges());

    FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(store->TransactionCountForTesting() == 1);
    FIC2_CHECK(store->ReadCountForTesting() == 0);
    FIC2_CHECK(!store->HasPendingChanges());

    const std::vector<std::wstring> list = PersistedList(ini);
    FIC2_REQUIRE(list.size() == RecentFiles::kMaxEntries);
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        FIC2_CHECK(list[i] == PathN(kAdds - 1 - static_cast<int>(i)));
    }

    // Nothing pending: no transaction.
    FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(store->TransactionCountForTesting() == 1);
}

// The flush reads the list fresh: entries another instance wrote after the
// Adds survive behind the local additions, case-insensitive duplicates are
// removed (the first one wins) and the list is capped.
FIC2_TEST(RecentFiles_FlushMergesIntoAFreshRead)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\seed.png");

    std::uint64_t now = 1000;
    auto a = MakeStore(ini, now);
    auto b = MakeStore(ini, now);

    ++now;
    a->Add(L"C:\\Pics\\Shared.PNG");
    ++now;
    a->Add(L"C:\\pics\\a2.png");
    ++now;
    a->Add(L"C:\\pics\\A1.png");
    ++now;
    a->Add(L"C:\\pics\\a1.png");     // re-selected: once, at the front

    for (int i = 0; i < 11; ++i)
    {
        ++now;
        b->Add(PathN(i));
    }
    ++now;
    b->Add(L"c:\\pics\\shared.png");
    FIC2_REQUIRE(b->Flush() == IniStore::WriteStatus::Committed);

    FIC2_REQUIRE(a->Flush() == IniStore::WriteStatus::Committed);
    const std::vector<std::wstring> list = PersistedList(ini);
    FIC2_REQUIRE(list.size() == RecentFiles::kMaxEntries);
    FIC2_CHECK(list[0] == L"C:\\pics\\a1.png");
    FIC2_CHECK(list[1] == L"C:\\pics\\a2.png");
    FIC2_CHECK(list[2] == L"C:\\Pics\\Shared.PNG");
    FIC2_CHECK(list[3] == PathN(10));      // B's list follows, without its "shared" entry
    FIC2_CHECK(list.back() == PathN(2));   // capped: B's oldest entries and the seed fall off
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        for (std::size_t j = i + 1; j < list.size(); ++j)
        {
            FIC2_CHECK(_wcsicmp(list[i].c_str(), list[j].c_str()) != 0);
        }
    }
}

// Clear epoch across two instances: A's addition at t1 does not come back after
// B's Clear at t2 > t1; A's addition at t3 > t2 is kept; a new instance
// (restart) sees the same list.
FIC2_TEST(RecentFiles_ClearEpochAcrossStores)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\seed.png");

    std::uint64_t now = 100;
    auto a = MakeStore(ini, now);
    auto b = MakeStore(ini, now);

    a->Add(L"C:\\t1.png");                 // t1 = 100
    now = 200;
    FIC2_REQUIRE(b->Clear() == IniStore::WriteStatus::Committed);   // t2 = 200
    FIC2_CHECK(PersistedClearedAt(ini) == 200);

    FIC2_CHECK(a->MenuEntries().empty());
    FIC2_REQUIRE(a->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(PersistedList(ini).empty());

    now = 300;
    a->Add(L"C:\\t3.png");                 // t3 = 300
    FIC2_REQUIRE(a->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(PersistedList(ini) == std::vector<std::wstring> { L"C:\\t3.png" });
    FIC2_CHECK(b->MenuEntries() == std::vector<std::wstring> { L"C:\\t3.png" });

    const RecentFilesStore restarted(ini);
    FIC2_CHECK(restarted.MenuEntries() == std::vector<std::wstring> { L"C:\\t3.png" });
    FIC2_CHECK(PersistedClearedAt(ini) == 200);
}

// Clear Recent commits at once (one transaction) and never lowers a persisted
// RecentFilesClearedAt.
FIC2_TEST(RecentFiles_ClearCommitsAtOnceWithNonDecreasingClearedAt)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\seed.png", L"500");

    std::uint64_t now = 300;   // behind the persisted Clear (another instance's clock)
    auto store = MakeStore(ini, now);
    store->Add(L"C:\\pending.png");
    FIC2_CHECK(store->Clear() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(store->TransactionCountForTesting() == 1);
    FIC2_CHECK(!store->HasPendingChanges());
    FIC2_CHECK(PersistedList(ini).empty());
    FIC2_CHECK(PersistedClearedAt(ini) == 500);
    FIC2_CHECK(IniStore::Load(ini).GetString(L"Window", L"ShowCmd") == L"1");

    now = 900;
    FIC2_CHECK(store->Clear() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(PersistedClearedAt(ini) == 900);
    FIC2_CHECK(IniStore::Load(ini).GetString(L"Session", L"RecentFilesClearedAt") == L"900");
}

// A Clear whose commit times out stays pending: the list is empty at once and
// the status is logged. The next flush retries with the click's time (not the
// retry's), takes the max with the persisted Clear, and filters the pending
// additions against that before committing.
FIC2_TEST(RecentFiles_ClearOnLockTimeoutRetriesWithTheClickTime)
{
    // No newer Clear meanwhile: the click's time is stored.
    {
        const TempDir temp;
        const std::wstring ini = temp.File(L"recent.ini");
        SeedIni(ini, L"C:\\old.png");
        const FileSnapshot before = Snapshot(ini);

        std::uint64_t now = 100;
        auto store = MakeStore(ini, now);
        store->Add(L"C:\\before.png");
        now = 200;
        LogCapture log;
        {
            const LockHolder lock(ini);
            FIC2_CHECK(store->Clear() == IniStore::WriteStatus::LockTimeout);
        }
        FIC2_CHECK(log.Contains("RecentFiles not saved: LockTimeout"));
        FIC2_CHECK(store->HasPendingChanges());
        FIC2_CHECK(Snapshot(ini) == before);
        FIC2_CHECK(store->MenuEntries().empty());   // the on-disk list is ignored

        now = 400;
        store->Add(L"C:\\after.png");
        FIC2_CHECK(store->MenuEntries() == std::vector<std::wstring> { L"C:\\after.png" });
        FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
        FIC2_CHECK(PersistedList(ini) == std::vector<std::wstring> { L"C:\\after.png" });
        FIC2_CHECK(PersistedClearedAt(ini) == 200);
        FIC2_CHECK(!store->HasPendingChanges());
    }

    // A newer Clear committed meanwhile: it stays the threshold, and the
    // addition between the two Clears is dropped.
    {
        const TempDir temp;
        const std::wstring ini = temp.File(L"recent.ini");
        SeedIni(ini, L"C:\\old.png");

        std::uint64_t now = 200;
        auto a = MakeStore(ini, now);
        auto b = MakeStore(ini, now);
        {
            const LockHolder lock(ini);
            FIC2_CHECK(a->Clear() == IniStore::WriteStatus::LockTimeout);
        }
        now = 220;
        a->Add(L"C:\\between.png");
        now = 250;
        FIC2_REQUIRE(b->Clear() == IniStore::WriteStatus::Committed);
        now = 300;
        a->Add(L"C:\\later.png");
        FIC2_CHECK(a->MenuEntries() == std::vector<std::wstring> { L"C:\\later.png" });
        FIC2_CHECK(a->Flush() == IniStore::WriteStatus::Committed);
        FIC2_CHECK(PersistedList(ini) == std::vector<std::wstring> { L"C:\\later.png" });
        FIC2_CHECK(PersistedClearedAt(ini) == 250);
    }
}

// A flush that times out or fails to commit keeps the pending additions and
// logs the status; a later successful flush persists them.
FIC2_TEST(RecentFiles_FailedFlushKeepsPendingUntilALaterFlush)
{
    for (const FailureKind kind : { FailureKind::LockTimeout, FailureKind::CommitFailed })
    {
        const TempDir temp;
        const std::wstring ini = temp.File(L"recent.ini");
        SeedIni(ini, L"C:\\old.png");
        const FileSnapshot before = Snapshot(ini);

        std::uint64_t now = 100;
        auto store = MakeStore(ini, now);
        store->Add(L"C:\\x.png");
        ++now;
        store->Add(L"C:\\y.png");

        LogCapture log;
        FIC2_CHECK(WithFailure(kind, ini, [&]() { return store->Flush(); }) == FailureStatus(kind));
        FIC2_CHECK(log.Contains(FailureText(kind)));
        FIC2_CHECK(store->HasPendingChanges());
        FIC2_CHECK(Snapshot(ini) == before);

        FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
        FIC2_CHECK((PersistedList(ini) == std::vector<std::wstring> { L"C:\\y.png", L"C:\\x.png", L"C:\\old.png" }));
        FIC2_CHECK(!store->HasPendingChanges());
    }
}

// The Open Recent menu merges the pending additions with the on-disk list
// using the same rule and threshold as the flush, and writes nothing.
FIC2_TEST(RecentFiles_MenuMergesWithoutWriting)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\b.png|C:\\c.png", L"100");
    const FileSnapshot before = Snapshot(ini);

    std::uint64_t now = 50;
    auto store = MakeStore(ini, now);
    store->Add(L"C:\\a.png");      // at the persisted Clear's time or before: not eligible
    now = 100;
    store->Add(L"C:\\a2.png");
    now = 150;
    store->Add(L"C:\\d.png");
    now = 160;
    store->Add(L"C:\\C.PNG");

    const std::vector<std::wstring> expected { L"C:\\C.PNG", L"C:\\d.png", L"C:\\b.png" };
    FIC2_CHECK(store->MenuEntries() == expected);
    FIC2_CHECK(store->MenuEntries() == expected);
    FIC2_CHECK(store->TransactionCountForTesting() == 0);
    FIC2_CHECK(Snapshot(ini) == before);
    FIC2_CHECK(store->HasPendingChanges());

    FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(PersistedList(ini) == expected);
    FIC2_CHECK(RecentFilesStore(ini).MenuEntries() == expected);
}

namespace
{
    // A's Clear at t1 fails; B commits a Clear at t2 > t1; A retries. The
    // stored threshold stays t2; C's unflushed additions at t1 < tMid <= t2
    // stay rejected, and C's addition at t3 > t2, flushed after A's retry, is
    // kept (menu, flush and restart).
    void RunThreeStoreRegression(FailureKind kind)
    {
        const TempDir temp;
        const std::wstring ini = temp.File(L"recent.ini");
        SeedIni(ini, L"C:\\seed.png");

        std::uint64_t now = 100;
        auto a = MakeStore(ini, now);
        auto b = MakeStore(ini, now);
        auto c = MakeStore(ini, now);

        FIC2_CHECK(WithFailure(kind, ini, [&]() { return a->Clear(); }) == FailureStatus(kind));   // t1 = 100
        now = 150;
        c->Add(L"C:\\mid1.png");
        now = 200;
        c->Add(L"C:\\mid2.png");
        FIC2_REQUIRE(b->Clear() == IniStore::WriteStatus::Committed);   // t2 = 200
        FIC2_CHECK(PersistedClearedAt(ini) == 200);

        now = 250;
        FIC2_CHECK(a->Flush() == IniStore::WriteStatus::Committed);   // A's retry
        FIC2_CHECK(PersistedClearedAt(ini) == 200);
        FIC2_CHECK(PersistedList(ini).empty());
        FIC2_CHECK(c->MenuEntries().empty());

        now = 300;
        c->Add(L"C:\\t3.png");
        FIC2_CHECK(c->MenuEntries() == std::vector<std::wstring> { L"C:\\t3.png" });
        FIC2_CHECK(c->Flush() == IniStore::WriteStatus::Committed);
        FIC2_CHECK(PersistedList(ini) == std::vector<std::wstring> { L"C:\\t3.png" });
        FIC2_CHECK(a->MenuEntries() == std::vector<std::wstring> { L"C:\\t3.png" });
        FIC2_CHECK(RecentFilesStore(ini).MenuEntries() == std::vector<std::wstring> { L"C:\\t3.png" });
        FIC2_CHECK(PersistedClearedAt(ini) == 200);
    }
}

FIC2_TEST(RecentFiles_ThreeStoreRetryKeepsTheNewerClear_LockTimeout)
{
    RunThreeStoreRegression(FailureKind::LockTimeout);
}

FIC2_TEST(RecentFiles_ThreeStoreRetryKeepsTheNewerClear_CommitFailed)
{
    RunThreeStoreRegression(FailureKind::CommitFailed);
}

// Accepted loss (documented): the persisted list has no per-entry times, so
// A's retried Clear also removes C's entry flushed after A's click.
FIC2_TEST(RecentFiles_RetriedClearRemovesEntriesFlushedMeanwhile)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\seed.png");

    std::uint64_t now = 100;
    auto a = MakeStore(ini, now);
    auto c = MakeStore(ini, now);

    FIC2_CHECK(WithFailure(FailureKind::LockTimeout, ini, [&]() { return a->Clear(); }) == IniStore::WriteStatus::LockTimeout);
    now = 300;
    c->Add(L"C:\\t3.png");
    FIC2_REQUIRE(c->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK((PersistedList(ini) == std::vector<std::wstring> { L"C:\\t3.png", L"C:\\seed.png" }));

    FIC2_CHECK(a->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(PersistedList(ini).empty());
    FIC2_CHECK(PersistedClearedAt(ini) == 100);
}

// The before-destroy flush on LockTimeout returns within the lock timeout,
// logs the status and drops what was pending.
FIC2_TEST(RecentFiles_ShutdownFlushOnLockTimeoutDropsPending)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\old.png");
    const FileSnapshot before = Snapshot(ini);

    std::uint64_t now = 100;
    auto store = MakeStore(ini, now);
    std::vector<bool> scheduled;
    store->SetFlushScheduler([&](bool arm) { scheduled.push_back(arm); });
    store->Add(L"C:\\x.png");

    LogCapture log;
    {
        const LockHolder lock(ini);
        const auto start = std::chrono::steady_clock::now();
        FIC2_CHECK(store->FlushBeforeDestroy() == IniStore::WriteStatus::LockTimeout);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        FIC2_CHECK(elapsed < std::chrono::milliseconds(kLockTimeoutMs + 500));
    }
    FIC2_CHECK(log.Contains("RecentFiles not saved: LockTimeout"));
    FIC2_CHECK(log.Contains("dropping 1 pending addition(s) at exit"));
    FIC2_CHECK(!store->HasPendingChanges());
    FIC2_CHECK((scheduled == std::vector<bool> { true, false }));

    const std::size_t transactions = store->TransactionCountForTesting();
    FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(store->TransactionCountForTesting() == transactions);
    FIC2_CHECK(Snapshot(ini) == before);
}

// After the before-destroy flush the timer is stopped, and a later Add neither
// re-arms it nor records anything, so no flush touches the INI.
FIC2_TEST(RecentFiles_AddAfterTheBeforeDestroyFlushIsIgnored)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\old.png");

    std::uint64_t now = 100;
    auto store = MakeStore(ini, now);
    std::vector<bool> scheduled;
    store->SetFlushScheduler([&](bool arm) { scheduled.push_back(arm); });
    store->Add(L"C:\\last.png");
    FIC2_CHECK(store->FlushBeforeDestroy() == IniStore::WriteStatus::Committed);
    FIC2_CHECK((PersistedList(ini) == std::vector<std::wstring> { L"C:\\last.png", L"C:\\old.png" }));
    FIC2_CHECK((scheduled == std::vector<bool> { true, false }));
    const FileSnapshot after = Snapshot(ini);
    const std::size_t transactions = store->TransactionCountForTesting();

    now = 200;
    store->Add(L"C:\\late.png");
    FIC2_CHECK((scheduled == std::vector<bool> { true, false }));
    FIC2_CHECK(!store->HasPendingChanges());
    FIC2_CHECK(store->Flush() == IniStore::WriteStatus::Committed);
    FIC2_CHECK(store->TransactionCountForTesting() == transactions);
    FIC2_CHECK(Snapshot(ini) == after);
}

// RecentFilesClearedAt is a decimal FILETIME; a missing or unparsable value
// reads as 0, so it never hides additions.
FIC2_TEST(RecentFiles_ClearedAtParsing)
{
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"") == 0);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"abc") == 0);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"-5") == 0);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"12x") == 0);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"99999999999999999999999") == 0);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"133000000000000000") == 133000000000000000ull);
    FIC2_CHECK(RecentFilesStore::ParseClearedAt(L"18446744073709551615") == 18446744073709551615ull);

    const TempDir temp;
    const std::wstring ini = temp.File(L"recent.ini");
    SeedIni(ini, L"C:\\old.png", L"garbage");
    std::uint64_t now = 1;
    auto store = MakeStore(ini, now);
    store->Add(L"C:\\new.png");
    FIC2_CHECK((store->MenuEntries() == std::vector<std::wstring> { L"C:\\new.png", L"C:\\old.png" }));
}
