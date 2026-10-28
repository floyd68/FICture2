#include "ArchiveEntryHandoff.h"

#include "AppLog.h"
#include "ArchiveCursorPool.h"
#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

struct ArchiveEntryHandoff::Core
{
    struct DemandRecord
    {
        std::uint64_t id { 0 };
        DemandKind kind { DemandKind::Thumbnail };
        bool served { false };   // its request read the entry
    };

    struct Parked
    {
        std::vector<uint8_t> bytes {};
        // The reader that extracted them: a hit is served only while the
        // archive cache still returns this reader (the file was not replaced).
        std::weak_ptr<Floar::IArchiveReader> reader {};
    };

    struct Entry
    {
        std::wstring innerPath {};   // normalized, for ExtractForward
        std::vector<DemandRecord> demands {};
        std::optional<Parked> parked {};

        bool HasMainDemand() const
        {
            return std::any_of(demands.begin(), demands.end(), [](const DemandRecord& d) { return d.kind == DemandKind::MainImage; });
        }

        // An unserved thumbnail demand, or null.
        const DemandRecord* WaitingThumbnail() const
        {
            for (const DemandRecord& d : demands)
            {
                if (d.kind == DemandKind::Thumbnail && !d.served)
                {
                    return &d;
                }
            }
            return nullptr;
        }
    };

    using Archive = std::unordered_map<std::wstring, Entry>;   // by ArchiveInnerPathKey

    mutable std::mutex mutex {};
    Limits limits {};
    std::unordered_map<std::wstring, Archive> archives {};   // by ArchiveReaderCache::Key
    std::uint64_t nextId { 0 };
    Stats stats {};

    Entry* FindLocked(const std::wstring& archive, const std::wstring& entry)
    {
        auto a = archives.find(archive);
        if (a == archives.end())
        {
            return nullptr;
        }
        auto e = a->second.find(entry);
        return e == a->second.end() ? nullptr : &e->second;
    }

    // Moves the parked bytes out of `entry` (into `doomed` or to the caller).
    std::vector<uint8_t> TakeParkedLocked(Entry& entry)
    {
        std::vector<uint8_t> bytes = std::move(entry.parked->bytes);
        entry.parked.reset();
        stats.parkedBytes -= bytes.size();
        --stats.parkedEntries;
        return bytes;
    }

    void EraseIfUnusedLocked(const std::wstring& archive, const std::wstring& entry)
    {
        auto a = archives.find(archive);
        if (a == archives.end())
        {
            return;
        }
        auto e = a->second.find(entry);
        if (e != a->second.end() && e->second.demands.empty() && !e->second.parked)
        {
            a->second.erase(e);
        }
        if (a->second.empty())
        {
            archives.erase(a);
        }
    }

    void Withdraw(const std::wstring& archive, const std::wstring& entryKey, std::uint64_t id)
    {
        std::vector<uint8_t> doomed;   // freed after the lock is dropped
        {
            std::lock_guard<std::mutex> lock(mutex);
            Entry* entry = FindLocked(archive, entryKey);
            if (!entry)
            {
                return;
            }
            auto it = std::find_if(entry->demands.begin(), entry->demands.end(), [id](const DemandRecord& d) { return d.id == id; });
            if (it == entry->demands.end())
            {
                return;
            }
            entry->demands.erase(it);
            --stats.demands;
            // Parked bytes belong to waiting thumbnail demands only.
            if (entry->parked && !entry->WaitingThumbnail())
            {
                doomed = TakeParkedLocked(*entry);
                ++stats.released;
            }
            EraseIfUnusedLocked(archive, entryKey);
        }
    }

    void NoteHeldLocked(std::uint64_t inHand)
    {
        stats.peakHeldBytes = (std::max)(stats.peakHeldBytes, stats.parkedBytes + inHand);
    }
};

namespace
{
    struct Candidate
    {
        std::wstring innerPath {};
        std::wstring key {};
        std::uint64_t demandId { 0 };
        size_t order { 0 };
    };
}

// -------------------------------------------------------------------------
// Demand

ArchiveEntryHandoff::Demand::Demand(Demand&& other) noexcept
    : m_core(std::move(other.m_core))
    , m_archive(std::move(other.m_archive))
    , m_entry(std::move(other.m_entry))
    , m_id(std::exchange(other.m_id, 0))
{
}

ArchiveEntryHandoff::Demand& ArchiveEntryHandoff::Demand::operator=(Demand&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        m_core = std::move(other.m_core);
        m_archive = std::move(other.m_archive);
        m_entry = std::move(other.m_entry);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

ArchiveEntryHandoff::Demand::~Demand()
{
    Reset();
}

void ArchiveEntryHandoff::Demand::Reset()
{
    if (m_id == 0)
    {
        return;
    }
    if (auto core = m_core.lock())
    {
        core->Withdraw(m_archive, m_entry, m_id);
    }
    m_core.reset();
    m_id = 0;
}

// -------------------------------------------------------------------------
// ArchiveEntryHandoff

ArchiveEntryHandoff::ArchiveEntryHandoff(Limits limits)
    : m_core(std::make_shared<Core>())
{
    m_core->limits = limits;
}

ArchiveEntryHandoff::~ArchiveEntryHandoff() = default;

ArchiveEntryHandoff& ArchiveEntryHandoff::Shared()
{
    static ArchiveEntryHandoff instance;
    return instance;
}

ArchiveEntryHandoff::Demand ArchiveEntryHandoff::Register(const std::wstring& path, DemandKind kind)
{
    const auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath || !vpath->IsInArchive())
    {
        return {};
    }

    Demand demand;
    demand.m_core = m_core;
    demand.m_archive = Floar::ArchiveReaderCache::Key(vpath->hostPath);
    demand.m_entry = Floar::ArchiveInnerPathKey(vpath->archiveInnerPath);

    std::lock_guard<std::mutex> lock(m_core->mutex);
    demand.m_id = ++m_core->nextId;
    Core::Entry& entry = m_core->archives[demand.m_archive][demand.m_entry];
    if (entry.innerPath.empty())
    {
        entry.innerPath = vpath->archiveInnerPath;
    }
    entry.demands.push_back({ demand.m_id, kind, false });
    Stats& stats = m_core->stats;
    ++stats.demands;
    stats.peakDemands = (std::max)(stats.peakDemands, static_cast<std::uint64_t>(stats.demands));
    return demand;
}

std::vector<uint8_t> ArchiveEntryHandoff::Read(const Floar::VirtualPath& path, std::stop_token stop, Floar::ArchiveReaderCache& cache)
{
    if (stop.stop_requested())
    {
        return {};
    }
    if (!path.IsInArchive())
    {
        return Floar::VirtualFileSystem::ReadFile(path, stop, cache);
    }

    const std::wstring archiveKey = Floar::ArchiveReaderCache::Key(path.hostPath);
    const std::wstring entryKey = Floar::ArchiveInnerPathKey(path.archiveInnerPath);

    // Take this read's demand (marking it served) and any parked bytes.
    bool demanded = false;
    bool main = false;
    std::optional<Core::Parked> taken;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        if (Core::Entry* entry = m_core->FindLocked(archiveKey, entryKey))
        {
            main = entry->HasMainDemand();
            const DemandKind preferred = main ? DemandKind::MainImage : DemandKind::Thumbnail;
            Core::DemandRecord* served = nullptr;
            for (Core::DemandRecord& d : entry->demands)
            {
                if (!d.served && (!served || (d.kind == preferred && served->kind != preferred)))
                {
                    served = &d;
                }
            }
            if (served)
            {
                served->served = true;
                demanded = true;
            }
            if (entry->parked)
            {
                Core::Parked parked;
                parked.reader = entry->parked->reader;
                parked.bytes = m_core->TakeParkedLocked(*entry);
                taken = std::move(parked);
            }
        }
    }

    if (taken)
    {
        const auto reader = cache.Get(path.hostPath, stop);
        if (stop.stop_requested())
        {
            return {};
        }
        if (reader && reader == taken->reader.lock())
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            ++m_core->stats.hits;
            return std::move(taken->bytes);
        }
        // The archive changed (or its reader was replaced) since the pass.
        std::lock_guard<std::mutex> lock(m_core->mutex);
        ++m_core->stats.droppedStale;
    }

    if (!demanded)
    {
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            ++m_core->stats.directReads;
        }
        return Floar::VirtualFileSystem::ReadFile(path, stop, cache);
    }
    if (main)
    {
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            ++m_core->stats.mainReads;
        }
        Floar::IArchiveReader::ExtractOptions high;
        high.priority = Floar::ExtractPriority::High;
        return Floar::VirtualFileSystem::ReadFile(path, stop, cache, high);
    }

    const auto reader = cache.Get(path.hostPath, stop);
    if (stop.stop_requested() || !reader)
    {
        return {};
    }
    const size_t targetOrder = reader->HasExpensiveRandomAccess()
        ? reader->ArchiveOrder(path.archiveInnerPath)
        : Floar::IArchiveReader::kNoArchiveOrder;
    if (targetOrder == Floar::IArchiveReader::kNoArchiveOrder)
    {
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            ++m_core->stats.directReads;
        }
        return Floar::VirtualFileSystem::ReadFile(path, stop, cache);
    }

    // The other waiting thumbnail entries of this archive that are not parked.
    std::vector<Candidate> candidates;
    size_t maxPassEntries = 0;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        // No more than could be parked now: with no room, collecting earlier
        // entries would only force a restart where the cursor could continue.
        const Limits& limits = m_core->limits;
        const Stats& stats = m_core->stats;
        const size_t room = (stats.parkedBytes >= limits.maxParkedBytes || stats.parkedEntries >= limits.maxParkedEntries)
            ? 0
            : limits.maxParkedEntries - stats.parkedEntries;
        maxPassEntries = (std::min)(limits.maxPassEntries, room);
        auto a = m_core->archives.find(archiveKey);
        if (a != m_core->archives.end())
        {
            for (const auto& [key, entry] : a->second)
            {
                if (key == entryKey || entry.parked)
                {
                    continue;
                }
                if (const Core::DemandRecord* waiting = entry.WaitingThumbnail())
                {
                    candidates.push_back({ entry.innerPath, key, waiting->id, 0 });
                }
            }
        }
    }
    // Those before the target in archive order (an immutable table: no lock).
    for (Candidate& c : candidates)
    {
        c.order = reader->ArchiveOrder(c.innerPath);
    }
    std::erase_if(candidates, [&](const Candidate& c) { return c.order == Floar::IArchiveReader::kNoArchiveOrder || c.order >= targetOrder; });
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& l, const Candidate& r) { return l.order < r.order; });
    if (candidates.size() > maxPassEntries)
    {
        candidates.erase(candidates.begin(), candidates.end() - static_cast<std::ptrdiff_t>(maxPassEntries));
    }

    std::vector<std::wstring> paths;
    std::unordered_map<std::wstring, size_t> byKey;
    paths.reserve(candidates.size() + 1);
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        paths.push_back(candidates[i].innerPath);
        byKey.emplace(candidates[i].key, i);
    }
    paths.push_back(path.archiveInnerPath);

    const std::weak_ptr<Floar::IArchiveReader> extractedBy = reader;
    std::vector<uint8_t> result;
    bool delivered = false;
    size_t parkedHere = 0;
    size_t droppedHere = 0;
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        ++m_core->stats.passes;
    }

    // Short and non-blocking (Floar's ForwardSink contract): parks or drops.
    const auto sink = [&](const std::wstring& entryPath, std::vector<uint8_t>&& data)
    {
        const std::wstring key = Floar::ArchiveInnerPathKey(entryPath);
        if (key == entryKey)
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            m_core->NoteHeldLocked(data.size());
            result = std::move(data);
            delivered = true;
            return false;   // the pass ends at the requested entry
        }
        auto it = byKey.find(key);
        if (it == byKey.end() || data.empty())
        {
            return true;
        }
        const Candidate& candidate = candidates[it->second];
        std::vector<uint8_t> doomed;   // freed after the lock is dropped
        {
            std::lock_guard<std::mutex> lock(m_core->mutex);
            Core& core = *m_core;
            core.NoteHeldLocked(data.size());
            Core::Entry* entry = core.FindLocked(archiveKey, candidate.key);
            const Core::DemandRecord* waiting = entry ? entry->WaitingThumbnail() : nullptr;
            const bool sameDemand = entry && !entry->parked && std::any_of(entry->demands.begin(), entry->demands.end(),
                [&](const Core::DemandRecord& d) { return d.id == candidate.demandId && !d.served; });
            if (!waiting || !sameDemand)
            {
                ++core.stats.droppedStale;   // withdrawn, served or replaced meanwhile
                doomed = std::move(data);
            }
            else if (data.size() > core.limits.maxParkedEntryBytes)
            {
                ++core.stats.droppedOversize;
                doomed = std::move(data);
            }
            else if (core.stats.parkedBytes + data.size() > core.limits.maxParkedBytes ||
                core.stats.parkedEntries + 1 > core.limits.maxParkedEntries)
            {
                ++core.stats.droppedFull;   // backpressure: read again by its own request
                doomed = std::move(data);
            }
            else
            {
                core.stats.parkedBytes += data.size();
                ++core.stats.parkedEntries;
                ++core.stats.parked;
                core.stats.peakParkedBytes = (std::max)(core.stats.peakParkedBytes, core.stats.parkedBytes);
                core.stats.peakParkedEntries = (std::max)(core.stats.peakParkedEntries, core.stats.parkedEntries);
                entry->parked = Core::Parked { std::move(data), extractedBy };
                ++parkedHere;
            }
        }
        if (!doomed.empty())
        {
            ++droppedHere;
        }
        return true;
    };

    const auto end = reader->ExtractForward(paths, stop, sink, Floar::IArchiveReader::ExtractOptions {});
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsedMs >= 100 || parkedHere > 0)
    {
        FIC2_LOG_INFO("[ArchiveHandoff] Pass to '{}': {} earlier demanded entr{} ({} parked, {} dropped) in {} ms",
            AppLog::ToUtf8(path.archiveInnerPath), candidates.size(), candidates.size() == 1 ? "y" : "ies", parkedHere, droppedHere, elapsedMs);
    }
    if (end == Floar::IArchiveReader::ExtractResult::Stopped || stop.stop_requested() || !delivered)
    {
        return {};
    }
    return result;
}

void ArchiveEntryHandoff::Clear()
{
    std::vector<std::vector<uint8_t>> doomed;
    std::lock_guard<std::mutex> lock(m_core->mutex);
    for (auto a = m_core->archives.begin(); a != m_core->archives.end();)
    {
        for (auto e = a->second.begin(); e != a->second.end();)
        {
            if (e->second.parked)
            {
                doomed.push_back(m_core->TakeParkedLocked(e->second));
            }
            e = e->second.demands.empty() ? a->second.erase(e) : std::next(e);
        }
        a = a->second.empty() ? m_core->archives.erase(a) : std::next(a);
    }
}

void ArchiveEntryHandoff::SetLimits(const Limits& limits)
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    m_core->limits = limits;
}

ArchiveEntryHandoff::Limits ArchiveEntryHandoff::CurrentLimits() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->limits;
}

ArchiveEntryHandoff::Stats ArchiveEntryHandoff::CurrentStats() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->stats;
}

void ArchiveEntryHandoff::ResetCounters()
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    Stats current;
    current.demands = m_core->stats.demands;
    current.parkedEntries = m_core->stats.parkedEntries;
    current.parkedBytes = m_core->stats.parkedBytes;
    current.peakDemands = current.demands;
    current.peakParkedEntries = current.parkedEntries;
    current.peakParkedBytes = current.parkedBytes;
    current.peakHeldBytes = current.parkedBytes;
    m_core->stats = current;
}

bool TrimArchiveCursorsOnLeave(const Floar::VirtualPath& from, const Floar::VirtualPath& to)
{
    const auto archiveOf = [](const Floar::VirtualPath& folder) -> std::wstring
    {
        if (folder.empty())
        {
            return {};
        }
        if (folder.IsInArchive() || Floar::ArchiveReaderFactory::HasArchiveExtension(folder.hostPath.filename().wstring()))
        {
            return Floar::HostPathKey(folder.hostPath);
        }
        return {};
    };

    const std::wstring left = archiveOf(from);
    if (left.empty() || left == archiveOf(to))
    {
        return false;
    }
    const size_t closed = Floar::ArchiveCursorPool::Shared()->TrimIdle();
    FIC2_LOG_INFO("[ArchiveHandoff] Left an archive: closed {} idle cursor(s)", closed);
    return true;
}
