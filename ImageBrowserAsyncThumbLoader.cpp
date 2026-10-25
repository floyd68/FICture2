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
    // Only chunks that add items need a UI update.
    if (!chunk.batch.empty() && model.Merge(chunk.batch) > 0)
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
