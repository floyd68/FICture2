#pragma once

#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <Windows.h>
#include <deque>
#include <functional>
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
};

class ImageBrowserAsyncThumbLoader
{
public:
    static void RegisterBrowser(const std::wstring& browserName, HANDLE readyEvent);
    static void UnregisterBrowser(const std::wstring& browserName);
    static void EnqueueChunk(const std::wstring& browserName, AsyncThumbListChunkPayload&& payload);
    static std::deque<AsyncThumbListChunkPayload> DequeueChunks(const std::wstring& browserName, HANDLE readyEvent);

    // Starts detached background enumeration and reports chunks via callback.
    static void StartEnumerate(
        const Floar::VirtualPath& folder,
        const std::function<void(std::vector<Floar::VirtualFileEntry>&&, bool completed)>& onChunk);

    static bool AcceptChunk(
        const AsyncThumbListChunkPayload& chunk,
        unsigned long long currentRequestId,
        const Floar::VirtualPath& currentFolder,
        bool currentShowNavItems);

    static bool ApplyChunkToProgressive(
        const AsyncThumbListChunkPayload& chunk,
        std::vector<Floar::VirtualFileEntry>& progressiveListedEntries,
        bool& progressiveUiDirty,
        bool& progressiveLoadCompleted);

    static bool ShouldApplyNow(
        bool progressiveLoadCompleted,
        unsigned long long nowMs,
        unsigned long long lastApplyMs,
        unsigned long long applyIntervalMs = 50ULL);
};
