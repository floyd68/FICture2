#pragma once

#include "ImageBrowserThumbModel.h"
#include "ImageSourceIdentity.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

struct AsyncThumbListChunkPayload
{
    unsigned long long requestId { 0 };
    Floar::VirtualPath folder {};
    Floar::VirtualPath preferSelectPath {};
    bool showNavItems { true };
    bool completed { false };
    std::vector<Floar::VirtualFileEntry> batch {};
    // Source identity of each batch entry, captured while listing (same order;
    // empty when none was captured).
    std::vector<ImageSourceIdentity> identities {};
};

// Folder-listing chunks (produced by FolderEnumeration.h) and how the browser
// merges them into its progressive thumbnail list.
class ImageBrowserAsyncThumbLoader
{
public:
    static bool AcceptChunk(
        const AsyncThumbListChunkPayload& chunk,
        unsigned long long currentRequestId,
        const Floar::VirtualPath& currentFolder,
        bool currentShowNavItems);

    static bool ApplyChunkToProgressive(
        const AsyncThumbListChunkPayload& chunk,
        ThumbItemModel& model,
        bool& progressiveUiDirty,
        bool& progressiveLoadCompleted);

    static bool ShouldApplyNow(
        bool progressiveLoadCompleted,
        unsigned long long nowMs,
        unsigned long long lastApplyMs,
        unsigned long long applyIntervalMs = 50ULL);
};

// One browser's progressive folder listing, without UI objects: which
// request is current, whether the strip has a staged update to apply (and
// when, throttled to one per kApplyIntervalMs until the listing completes),
// and which listed entry is the preferred selection. The browser owns the
// folder lane that produces the chunks, rebuilds the strip and requests
// frames.
class ThumbListingSession
{
public:
    static constexpr unsigned long long kApplyIntervalMs = 50ULL;

    // A listing starts (superseding the previous one); returns its request
    // id. Progress and the preferred-selection key are cleared.
    unsigned long long Start(const Floar::VirtualPath& preferSelectPath, unsigned long long nowMs);

    unsigned long long RequestId() const { return m_requestId; }
    // True from Start until the update that applies the completed listing.
    bool Loading() const { return m_loading; }
    const Floar::VirtualPath& PreferSelectPath() const { return m_preferSelectPath; }
    unsigned long long StartMs() const { return m_startMs; }

    // Stages the chunks of the current request (same request id, folder and
    // nav-item setting) in `model` and resolves the preferred key from them.
    // Returns true if any chunk was accepted.
    bool Merge(const std::deque<AsyncThumbListChunkPayload>& chunks,
        const Floar::VirtualPath& currentFolder, bool showNavItems, ThumbItemModel& model);

    // A staged update (or the completion) waits to be applied.
    bool UpdatePending() const { return m_updatePending; }
    // Every chunk of the current request has arrived.
    bool Completed() const { return m_completed; }

    // After chunks were merged: apply at once when the listing completed or
    // the throttle interval has passed.
    bool ShouldApplyNow(unsigned long long nowMs) const;
    // The throttled path (every frame while loading): nothing to do, apply
    // now, or wait for the interval (keep requesting frames).
    enum class Throttle
    {
        None,
        Apply,
        Wait,
    };
    Throttle ThrottledUpdate(unsigned long long nowMs) const;
    // The strip applied the staged update at `nowMs`. Returns true when it
    // was the completed listing (the load is over).
    bool Applied(unsigned long long nowMs);

    // The preferred-selection key: the lookup key of the listed entry that is
    // `preferSelectPath` (compared as paths, as the strip always has), found
    // once per listing, so a path spelled differently from the listing still
    // selects its item.
    void ResetPreferKey() { m_preferKey.clear(); }
    void ResolvePreferKey(const Floar::VirtualPath& preferSelectPath, const std::vector<Floar::VirtualFileEntry>& entries);
    // The display index of `preferSelectPath` in `model` (or of the resolved
    // key), or 0 when it is not listed.
    size_t PreferredIndex(const ThumbItemModel& model, const Floar::VirtualPath& preferSelectPath) const;

private:
    unsigned long long m_requestId { 0 };
    bool m_loading { false };
    Floar::VirtualPath m_preferSelectPath {};
    std::wstring m_preferKey {};
    bool m_updatePending { false };
    bool m_completed { false };
    unsigned long long m_lastApplyMs { 0 };
    unsigned long long m_startMs { 0 };
};
