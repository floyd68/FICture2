#pragma once

#include "IniStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace RecentFiles
{
    constexpr std::size_t kMaxEntries = 12;
    // Quiet time after the last Add before the pending additions are flushed.
    constexpr UINT kFlushDelayMs = 1000;

    // Menu label: trim long paths and escape '&' mnemonics.
    std::wstring MenuLabel(const std::wstring& path);
}

// The process's recent-files list (Session/RecentFiles), owned by the app's
// Ficture2Backplate and used on the UI thread only.
//
// - Add records the selection in memory (newest first) and (re)arms the
//   debounce timer through the flush scheduler; it never reads or writes the
//   INI. The timer handler calls Flush().
// - Flush is one IniStore transaction: it reads RecentFiles and
//   RecentFilesClearedAt fresh, drops pending additions at or before the
//   effective Clear threshold (the larger of the persisted RecentFilesClearedAt
//   and a local Clear that has not been committed yet), puts the rest in front
//   of the persisted list (which a pending local Clear empties), removes
//   case-insensitive duplicates (the first one wins), caps the list at
//   kMaxEntries and commits. On LockTimeout or CommitFailed the status is
//   logged and everything pending is kept for the next flush.
// - Clear drops the pending additions and commits an empty list with
//   RecentFilesClearedAt = max(persisted, click time) at once; if that fails
//   the Clear stays pending and the next flush retries it with the original
//   click time, so another instance's newer Clear is never undone.
// - MenuEntries reads the INI and merges the pending additions the same way
//   without writing.
// - FlushBeforeDestroy is the last flush: it stops the timer, and what it
//   cannot commit is logged and dropped; Add is ignored from then on.
//
// Accepted risks: the Clear rule compares wall-clock UTC times across
// processes, so a system clock adjustment between an addition in one process
// and a Clear in another can misorder them (an addition just before the Clear
// may survive, one just after may be dropped). The persisted list has no
// per-entry times, so a Clear retried after a failed commit also removes the
// entries other instances flushed between the click and the retry. Entries of
// different instances are ordered by flush, not by selection time.
class RecentFilesStore
{
public:
    // UTC FILETIME as a 64-bit count of 100 ns intervals.
    using Clock = std::function<std::uint64_t()>;
    // Called with true to (re)arm the debounce timer and false to stop it.
    using FlushScheduler = std::function<void(bool arm)>;

    // An empty clock uses GetSystemTimePreciseAsFileTime.
    explicit RecentFilesStore(
        std::wstring iniPath,
        Clock clock = {},
        DWORD lockTimeoutMs = IniStore::kDefaultLockTimeoutMs);

    RecentFilesStore(const RecentFilesStore&) = delete;
    RecentFilesStore& operator=(const RecentFilesStore&) = delete;

    void SetFlushScheduler(FlushScheduler scheduler);

    void Add(const std::wstring& path);
    IniStore::WriteStatus Flush();
    IniStore::WriteStatus Clear();
    std::vector<std::wstring> MenuEntries() const;
    IniStore::WriteStatus FlushBeforeDestroy();

    bool HasPendingChanges() const { return !m_pending.empty() || m_pendingClear.has_value(); }

    // INI reads and transactions this store has performed (tests).
    std::size_t ReadCountForTesting() const { return m_reads; }
    std::size_t TransactionCountForTesting() const { return m_transactions; }

    // RecentFilesClearedAt: decimal FILETIME; missing or unparsable reads as 0.
    static std::uint64_t ParseClearedAt(const std::wstring& text);

private:
    struct Pending
    {
        std::wstring path;
        std::uint64_t time = 0;
    };

    std::vector<std::wstring> Merge(const std::vector<std::wstring>& persisted, std::uint64_t persistedClearedAt) const;
    std::uint64_t EffectiveClear(std::uint64_t persistedClearedAt) const;

    std::wstring m_iniPath;
    Clock m_clock;
    DWORD m_lockTimeoutMs = IniStore::kDefaultLockTimeoutMs;
    FlushScheduler m_scheduler;
    std::vector<Pending> m_pending;                 // newest first, distinct paths
    std::optional<std::uint64_t> m_pendingClear;    // click time of an uncommitted Clear
    std::uint64_t m_lastClear = 0;                  // click time of the last local Clear
    bool m_closed = false;
    mutable std::size_t m_reads = 0;
    std::size_t m_transactions = 0;
};
