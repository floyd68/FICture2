#include "ImageBrowserAsyncThumbLoader.h"
#include "CommonUtil.h"

namespace
{
    bool PathEqualsInsensitive(const Floar::VirtualPath& a, const Floar::VirtualPath& b)
    {
        return CommonUtil::PathEqualsInsensitive(
            a.hostPath, a.archiveInnerPath,
            b.hostPath, b.archiveInnerPath);
    }
}

bool ImageBrowserAsyncThumbLoader::AcceptChunk(
    const AsyncThumbListChunkPayload& chunk,
    unsigned long long currentRequestId,
    const Floar::VirtualPath& currentFolder,
    bool currentShowNavItems)
{
    return chunk.requestId == currentRequestId &&
        PathEqualsInsensitive(chunk.folder, currentFolder) &&
        chunk.showNavItems == currentShowNavItems;
}

bool ImageBrowserAsyncThumbLoader::ApplyChunkToProgressive(
    const AsyncThumbListChunkPayload& chunk,
    ThumbItemModel& model,
    bool& progressiveUiDirty,
    bool& progressiveLoadCompleted)
{
    bool changed = false;
    // Only chunks that add items need a UI update, which commits them.
    if (!chunk.batch.empty() && model.Stage(chunk.batch, chunk.identities) > 0)
    {
        progressiveUiDirty = true;
        changed = true;
    }

    if (chunk.completed)
    {
        progressiveLoadCompleted = true;
        progressiveUiDirty = true;
        changed = true;
    }

    return changed;
}

bool ImageBrowserAsyncThumbLoader::ShouldApplyNow(
    bool progressiveLoadCompleted,
    unsigned long long nowMs,
    unsigned long long lastApplyMs,
    unsigned long long applyIntervalMs)
{
    return progressiveLoadCompleted || (nowMs - lastApplyMs >= applyIntervalMs);
}

unsigned long long ThumbListingSession::Start(const Floar::VirtualPath& preferSelectPath, unsigned long long nowMs)
{
    ++m_requestId;
    m_loading = true;
    m_startMs = nowMs;
    m_preferSelectPath = preferSelectPath;
    m_preferKey.clear();
    m_updatePending = false;
    m_completed = false;
    m_lastApplyMs = 0;
    return m_requestId;
}

bool ThumbListingSession::Merge(const std::deque<AsyncThumbListChunkPayload>& chunks,
    const Floar::VirtualPath& currentFolder, bool showNavItems, ThumbItemModel& model)
{
    bool anyAccepted = false;
    for (const auto& chunk : chunks)
    {
        if (!ImageBrowserAsyncThumbLoader::AcceptChunk(chunk, m_requestId, currentFolder, showNavItems))
        {
            continue;
        }
        anyAccepted = true;
        ResolvePreferKey(m_preferSelectPath, chunk.batch);
        (void)ImageBrowserAsyncThumbLoader::ApplyChunkToProgressive(chunk, model, m_updatePending, m_completed);
    }
    return anyAccepted;
}

bool ThumbListingSession::ShouldApplyNow(unsigned long long nowMs) const
{
    return ImageBrowserAsyncThumbLoader::ShouldApplyNow(m_completed, nowMs, m_lastApplyMs, kApplyIntervalMs);
}

ThumbListingSession::Throttle ThumbListingSession::ThrottledUpdate(unsigned long long nowMs) const
{
    if (!m_updatePending || m_completed)
    {
        return Throttle::None;
    }
    return (nowMs - m_lastApplyMs >= kApplyIntervalMs) ? Throttle::Apply : Throttle::Wait;
}

bool ThumbListingSession::Applied(unsigned long long nowMs)
{
    m_updatePending = false;
    m_lastApplyMs = nowMs;
    if (m_completed)
    {
        m_loading = false;
    }
    return m_completed;
}

void ThumbListingSession::ResolvePreferKey(const Floar::VirtualPath& preferSelectPath,
    const std::vector<Floar::VirtualFileEntry>& entries)
{
    if (preferSelectPath.empty() || !m_preferKey.empty())
    {
        return;
    }
    for (const auto& entry : entries)
    {
        if (PathEqualsInsensitive(entry.path, preferSelectPath))
        {
            m_preferKey = ThumbItemModel::LookupKeyFor(entry.path);
            return;
        }
    }
}

size_t ThumbListingSession::PreferredIndex(const ThumbItemModel& model, const Floar::VirtualPath& preferSelectPath) const
{
    if (preferSelectPath.empty())
    {
        return 0;
    }
    auto index = model.IndexOf(ThumbItemModel::LookupKeyFor(preferSelectPath));
    if (!index && !m_preferKey.empty())
    {
        index = model.IndexOf(m_preferKey);
    }
    return index.value_or(0);
}
