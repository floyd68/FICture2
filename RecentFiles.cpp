#include "RecentFiles.h"
#include "AppLog.h"
#include "IniStore.h"
#include "IniStoreLog.h"

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    constexpr const wchar_t* kSection = L"Session";
    constexpr const wchar_t* kListKey = L"RecentFiles";
    constexpr const wchar_t* kClearedAtKey = L"RecentFilesClearedAt";

    std::uint64_t SystemTimeNow()
    {
        FILETIME now {};
        GetSystemTimePreciseAsFileTime(&now);
        return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }

    bool SamePath(const std::wstring& a, const std::wstring& b)
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    // Appends `path` unless the list already has it (first occurrence wins).
    void AppendDistinct(std::vector<std::wstring>& list, const std::wstring& path)
    {
        if (list.size() < RecentFiles::kMaxEntries &&
            std::none_of(list.begin(), list.end(), [&](const std::wstring& p) { return SamePath(p, path); }))
        {
            list.push_back(path);
        }
    }
}

namespace RecentFiles
{
    std::wstring MenuLabel(const std::wstring& path)
    {
        constexpr std::size_t kMax = 64;
        std::wstring label = path.size() <= kMax
            ? path
            : L"..." + path.substr(path.size() - (kMax - 3));

        std::wstring escaped;
        escaped.reserve(label.size() + 4);
        for (const wchar_t c : label)
        {
            escaped += c;
            if (c == L'&')
            {
                escaped += L'&';
            }
        }
        return escaped;
    }
}

RecentFilesStore::RecentFilesStore(std::wstring iniPath, Clock clock, DWORD lockTimeoutMs)
    : m_iniPath(std::move(iniPath))
    , m_clock(clock ? std::move(clock) : Clock(&SystemTimeNow))
    , m_lockTimeoutMs(lockTimeoutMs)
{
}

void RecentFilesStore::SetFlushScheduler(FlushScheduler scheduler)
{
    m_scheduler = std::move(scheduler);
}

void RecentFilesStore::Add(const std::wstring& path)
{
    if (m_closed || m_iniPath.empty() || path.empty())
    {
        return;
    }

    // Selections after a local Clear stay after it even if the clock has not
    // advanced (or went back) since the click.
    const std::uint64_t time = std::max(m_clock(), m_lastClear + 1);
    m_pending.erase(
        std::remove_if(m_pending.begin(), m_pending.end(), [&](const Pending& p) { return SamePath(p.path, path); }),
        m_pending.end());
    m_pending.insert(m_pending.begin(), Pending { path, time });
    // Older pending entries could only end up behind these in any merge.
    if (m_pending.size() > RecentFiles::kMaxEntries)
    {
        m_pending.resize(RecentFiles::kMaxEntries);
    }

    if (m_scheduler)
    {
        m_scheduler(true);
    }
}

std::uint64_t RecentFilesStore::ParseClearedAt(const std::wstring& text)
{
    if (text.empty() || text.front() < L'0' || text.front() > L'9')
    {
        return 0;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = std::wcstoull(text.c_str(), &end, 10);
    return (errno == 0 && end != nullptr && *end == L'\0') ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t RecentFilesStore::EffectiveClear(std::uint64_t persistedClearedAt) const
{
    // A retried local Clear never lowers a newer Clear another instance
    // committed meanwhile, so it cannot revalidate additions that Clear removed.
    return std::max(persistedClearedAt, m_pendingClear.value_or(0));
}

std::vector<std::wstring> RecentFilesStore::Merge(
    const std::vector<std::wstring>& persisted,
    std::uint64_t persistedClearedAt) const
{
    const std::uint64_t threshold = EffectiveClear(persistedClearedAt);
    std::vector<std::wstring> merged;
    for (const Pending& p : m_pending)
    {
        if (p.time > threshold)
        {
            AppendDistinct(merged, p.path);
        }
    }
    // A local Clear that has not been committed yet empties the persisted list.
    if (!m_pendingClear)
    {
        for (const std::wstring& path : persisted)
        {
            AppendDistinct(merged, path);
        }
    }
    return merged;
}

IniStore::WriteStatus RecentFilesStore::Flush()
{
    if (m_iniPath.empty() || !HasPendingChanges())
    {
        return IniStore::WriteStatus::Committed;
    }

    ++m_transactions;
    const IniStore::WriteStatus status = IniStore::Transact(m_iniPath, [this](IniStore& ini)
    {
        const std::uint64_t persistedClearedAt = ParseClearedAt(ini.GetString(kSection, kClearedAtKey));
        const std::vector<std::wstring> persisted = IniStore::SplitPipeList(ini.GetString(kSection, kListKey));
        ini.Put(kSection, kListKey, IniStore::JoinPipeList(Merge(persisted, persistedClearedAt)));
        if (m_pendingClear)
        {
            ini.Put(kSection, kClearedAtKey, std::to_wstring(EffectiveClear(persistedClearedAt)));
        }
    }, m_lockTimeoutMs);

    if (status == IniStore::WriteStatus::Committed)
    {
        m_pending.clear();
        m_pendingClear.reset();
    }
    return LogIniWrite(status, "RecentFiles");
}

IniStore::WriteStatus RecentFilesStore::Clear()
{
    if (m_closed || m_iniPath.empty())
    {
        return IniStore::WriteStatus::Committed;
    }

    // The click's time is the Clear's candidate, also when it is retried later.
    m_lastClear = std::max(m_clock(), m_lastClear + 1);
    m_pendingClear = m_lastClear;
    m_pending.clear();
    return Flush();
}

std::vector<std::wstring> RecentFilesStore::MenuEntries() const
{
    if (m_iniPath.empty())
    {
        return {};
    }

    ++m_reads;
    const IniStore ini = IniStore::Load(m_iniPath, m_lockTimeoutMs);
    return Merge(
        IniStore::SplitPipeList(ini.GetString(kSection, kListKey)),
        ParseClearedAt(ini.GetString(kSection, kClearedAtKey)));
}

IniStore::WriteStatus RecentFilesStore::FlushBeforeDestroy()
{
    if (m_scheduler)
    {
        m_scheduler(false);
    }
    m_closed = true;

    // Bounded by the lock timeout; nothing is left to retry after this one.
    const IniStore::WriteStatus status = Flush();
    if (status != IniStore::WriteStatus::Committed)
    {
        FIC2_LOG_WARN("[Ini] RecentFiles: dropping {} pending addition(s){} at exit",
            m_pending.size(),
            m_pendingClear ? " and a pending Clear" : "");
        m_pending.clear();
        m_pendingClear.reset();
    }
    return status;
}
