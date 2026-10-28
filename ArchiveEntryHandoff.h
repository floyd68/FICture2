#pragma once

// Batched reads of demanded thumbnails in solid archives (#43).
//
// Reading one entry of a solid 7z or RAR decompresses every entry before it.
// ImageCore reads each thumbnail on its own (FloarPathByteSource::ReadAll on
// the serialized prefetch thread), in request-priority order, not in archive
// order. This class lets one archive pass serve several thumbnails:
//
// - Demand window: a request registers a Demand for its path while it is
//   outstanding (ImageAsyncBinding holds one per request: from dispatch until
//   it completes, is superseded or canceled). The thumbnail demand is
//   therefore exactly the strip's outstanding thumbnail requests - the visible
//   items plus the bounded prefetch margin of #46 - never every image of a
//   folder, and it follows the viewport as tiles are bound and recycled.
// - Forward pass: a thumbnail read of a demanded entry that is not parked
//   reads, in one ExtractForward call on the archive's forward cursor
//   (Floar's ArchiveCursorPool), every other demanded thumbnail entry that
//   comes before it in archive order and is not parked yet, parks those, and
//   ends at the requested entry, which it returns. Later demanded entries are
//   read by their own requests, which continue the retained cursor (no
//   reopen, no replay); an entry behind every retained cursor restarts from
//   the start of the archive, and the pass then also collects every demanded
//   entry before it, so one restart serves them all. Ending at the requested
//   entry keeps thumbnails appearing progressively: each one is decoded while
//   the next read continues the cursor.
// - Handoff cache: parked bytes are keyed by archive (ArchiveReaderCache::Key)
//   and entry (ArchiveInnerPathKey) and owned by their demand. A read of a
//   parked entry moves the bytes out (no archive access) once the reader the
//   cache returns is still the one that extracted them; otherwise they are
//   dropped and the entry is read again. Parked bytes are released when the
//   last thumbnail demand of the entry is withdrawn.
// - Never waits on a consumer: the pass runs inside the requesting read, on
//   the caller's thread, and only extracts and parks. When parking would
//   exceed the budget, the entry is dropped instead (backpressure: it is read
//   again by its own request); nothing waits for a parked entry to be taken.
// - Main images: a read of a path with a MainImage demand is a single-entry
//   read with ExtractPriority::High (cursor admission ahead of thumbnail
//   cursors); it never runs or waits for a thumbnail pass. Reads without a
//   demand, plain files and archives with cheap random access (ZIP, BA2, BSA)
//   are read exactly as before (VirtualFileSystem::ReadFile, Normal priority).
// - Cancellation and generations: the pass runs on the requesting read's stop
//   token (the request's #45 token), so a canceled request ends it at the
//   next entry or block boundary. Each demand has its own id; a pass parks an
//   entry only while the demand it saw when it started is still registered
//   and unserved, so bytes extracted for a withdrawn demand (a recycled tile,
//   a folder that was left) are dropped, never handed to a newer request.
//   There is no thread of its own and no detached work.
//
// Byte budget (Limits):
// - Parked bytes: at most maxParkedBytes and maxParkedEntries.
// - Active extraction: one entry per running read, held by Floar while it is
//   decompressed and then by the sink until it is parked, dropped or
//   returned; Stats::peakHeldBytes measures parked plus that entry.
// - Oversize entries: an entry larger than maxParkedEntryBytes is never
//   parked; it is read by its own request, which returns it directly (a
//   one-entry allowance, bounded by Floar's 1 GiB per-entry limit).
// - Decode-held input: bytes returned to ImageCore are owned by ImageCore and
//   bounded by its own count bounds, not by this budget: the prefetch stage
//   holds at most its prefetch cap of thumbnail tasks' bytes (FileByteCache
//   shares the same buffers and caps its entries at that cap), plus one read
//   in progress, plus one full-resolution input per decode worker; each is at
//   most 1 GiB (ReadWholeFileBytes). The two budgets are separate and add up:
//   handoff (maxParkedBytes + one entry per running read) + ImageCore's
//   count-bounded inputs. ImageCore#15 may add a byte bound to the second
//   part; it does not change this one. A parked entry moves to ImageCore when
//   it is read, so no byte is counted by both at once.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace Floar
{
class ArchiveReaderCache;
struct VirtualPath;
}

class ArchiveEntryHandoff
{
public:
    enum class DemandKind
    {
        Thumbnail,   // batched in archive order on a Normal cursor
        MainImage,   // read alone at High priority
    };

    struct Limits
    {
        std::uint64_t maxParkedBytes { 64ull * 1024 * 1024 };
        size_t maxParkedEntries { 256 };
        // Larger entries are never parked (oversize policy).
        std::uint64_t maxParkedEntryBytes { 16ull * 1024 * 1024 };
        // Demanded entries before the requested one that one pass collects
        // (the nearest ones when there are more).
        size_t maxPassEntries { 128 };
    };

    struct Stats
    {
        size_t demands { 0 };                 // registered, not withdrawn
        size_t parkedEntries { 0 };
        std::uint64_t parkedBytes { 0 };
        size_t peakParkedEntries { 0 };
        std::uint64_t peakParkedBytes { 0 };
        std::uint64_t peakHeldBytes { 0 };    // parked + the entry a pass holds
        std::uint64_t peakDemands { 0 };

        std::uint64_t passes { 0 };           // forward passes for a thumbnail read
        std::uint64_t hits { 0 };             // reads served from parked bytes
        std::uint64_t parked { 0 };           // entries parked by passes
        std::uint64_t droppedFull { 0 };      // not parked: the budget was full
        std::uint64_t droppedOversize { 0 };  // not parked: larger than maxParkedEntryBytes
        std::uint64_t droppedStale { 0 };     // not parked or not served: demand withdrawn/replaced, or the archive changed
        std::uint64_t released { 0 };         // parked entries released with their last demand
        std::uint64_t mainReads { 0 };        // High-priority main-image reads
        std::uint64_t directReads { 0 };      // reads as before: no demand, or cheap random access
    };

    struct Core;

    // A registered demand; withdrawn when destroyed or reset. Move-only.
    class Demand
    {
    public:
        Demand() = default;
        Demand(Demand&& other) noexcept;
        Demand& operator=(Demand&& other) noexcept;
        Demand(const Demand&) = delete;
        Demand& operator=(const Demand&) = delete;
        ~Demand();

        void Reset();
        explicit operator bool() const { return m_id != 0; }

    private:
        friend class ArchiveEntryHandoff;
        std::weak_ptr<Core> m_core {};
        std::wstring m_archive {};
        std::wstring m_entry {};
        std::uint64_t m_id { 0 };
    };

    explicit ArchiveEntryHandoff(Limits limits = {});
    ~ArchiveEntryHandoff();
    ArchiveEntryHandoff(const ArchiveEntryHandoff&) = delete;
    ArchiveEntryHandoff& operator=(const ArchiveEntryHandoff&) = delete;

    // The instance behind FloarPathByteSource's default constructor and
    // ImageAsyncBinding's demands.
    static ArchiveEntryHandoff& Shared();

    // Registers that a request of `kind` will read `path` (a display path).
    // Paths outside archives get an empty demand. Lexical only: no file access.
    Demand Register(const std::wstring& path, DemandKind kind);

    // FloarPathByteSource::ReadAll: the bytes of `path`, empty on failure or
    // once `stop` is requested (nothing partial). See the class comment.
    std::vector<uint8_t> Read(const Floar::VirtualPath& path, std::stop_token stop, Floar::ArchiveReaderCache& cache);

    // Drops every parked entry (shutdown, or tests between cases). Demands stay.
    void Clear();

    void SetLimits(const Limits& limits);
    Limits CurrentLimits() const;
    Stats CurrentStats() const;
    // Resets the counters and peaks (not the current values).
    void ResetCounters();

private:
    std::shared_ptr<Core> m_core;
};

// Closes Floar's idle forward cursors (ArchiveCursorPool::Shared()->TrimIdle)
// when a browser lists `to` after `from` and `from` was in (or was) an archive
// that `to` is not in. Returns true when it trimmed. Idle cursors hold no file
// handle, but they hold decompressor memory that the archive left behind no
// longer needs.
bool TrimArchiveCursorsOnLeave(const Floar::VirtualPath& from, const Floar::VirtualPath& to);
