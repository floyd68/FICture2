#pragma once

#include "ImageBrowserThumbModel.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <vector>

struct AsyncThumbListChunkPayload
{
    unsigned long long requestId { 0 };
    Floar::VirtualPath folder {};
    Floar::VirtualPath preferSelectPath {};
    bool showNavItems { true };
    bool completed { false };
    std::vector<Floar::VirtualFileEntry> batch {};
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
