#include "ImageBrowserAsyncThumbLoader.h"

#include <filesystem>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
    std::wstring ToLower(std::wstring s)
    {
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(towlower(c));
        }
        return s;
    }

    bool PathEqualsInsensitive(const VirtualPath& a, const VirtualPath& b)
    {
        return ToLower(a.GetDisplayPath()) == ToLower(b.GetDisplayPath());
    }

    std::mutex g_asyncThumbListMutex;
    std::unordered_map<std::wstring, std::deque<AsyncThumbListChunkPayload>> g_asyncThumbListChunksByBrowser {};
    std::unordered_map<std::wstring, HANDLE> g_asyncThumbReadyEventByBrowser {};
}

void ImageBrowserAsyncThumbLoader::RegisterBrowser(const std::wstring& browserName, HANDLE readyEvent)
{
    std::lock_guard<std::mutex> lock(g_asyncThumbListMutex);
    g_asyncThumbReadyEventByBrowser[browserName] = readyEvent;
}

void ImageBrowserAsyncThumbLoader::UnregisterBrowser(const std::wstring& browserName)
{
    std::lock_guard<std::mutex> lock(g_asyncThumbListMutex);
    g_asyncThumbListChunksByBrowser.erase(browserName);
    g_asyncThumbReadyEventByBrowser.erase(browserName);
}

void ImageBrowserAsyncThumbLoader::EnqueueChunk(const std::wstring& browserName, AsyncThumbListChunkPayload&& payload)
{
    HANDLE eventHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_asyncThumbListMutex);
        auto& queue = g_asyncThumbListChunksByBrowser[browserName];
        queue.push_back(std::move(payload));
        auto eventIt = g_asyncThumbReadyEventByBrowser.find(browserName);
        if (eventIt != g_asyncThumbReadyEventByBrowser.end())
        {
            eventHandle = eventIt->second;
        }
    }

    if (eventHandle != nullptr)
    {
        SetEvent(eventHandle);
    }
}

std::deque<AsyncThumbListChunkPayload> ImageBrowserAsyncThumbLoader::DequeueChunks(
    const std::wstring& browserName,
    HANDLE readyEvent)
{
    std::deque<AsyncThumbListChunkPayload> chunks {};
    {
        std::lock_guard<std::mutex> lock(g_asyncThumbListMutex);
        auto it = g_asyncThumbListChunksByBrowser.find(browserName);
        if (it != g_asyncThumbListChunksByBrowser.end())
        {
            chunks = std::move(it->second);
            g_asyncThumbListChunksByBrowser.erase(it);
        }
    }

    if (readyEvent != nullptr && chunks.empty())
    {
        ResetEvent(readyEvent);
    }

    return chunks;
}

void ImageBrowserAsyncThumbLoader::StartEnumerate(
    const VirtualPath& folder,
    const std::function<void(std::vector<VirtualFileEntry>&&, bool completed)>& onChunk)
{
    std::thread([folder, onChunk]()
    {
        if (!onChunk)
        {
            return;
        }

        if (!folder.IsInArchive() && !folder.IsArchiveFile())
        {
            std::error_code ec;
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            std::filesystem::directory_iterator it(folder.hostPath, options, ec);
            if (ec)
            {
                onChunk({}, true);
                return;
            }

            constexpr size_t kBatchSize = 64;
            std::vector<VirtualFileEntry> batch {};
            batch.reserve(kBatchSize);
            const std::filesystem::directory_iterator end {};
            for (; it != end; it.increment(ec))
            {
                if (ec)
                {
                    ec.clear();
                    continue;
                }

                const std::filesystem::directory_entry& entry = *it;
                std::error_code typeEc;
                const bool isDir = entry.is_directory(typeEc);
                if (typeEc)
                {
                    continue;
                }

                batch.emplace_back(VirtualPath(entry.path()), isDir, 0, 0);
                if (batch.size() >= kBatchSize)
                {
                    onChunk(std::move(batch), false);
                    batch.clear();
                    batch.reserve(kBatchSize);
                }
            }

            onChunk(std::move(batch), true);
            return;
        }

        // Archive listing is currently produced as a single snapshot.
        onChunk(VirtualFileSystem::ListDirectory(folder), true);
    }).detach();
}

bool ImageBrowserAsyncThumbLoader::AcceptChunk(
    const AsyncThumbListChunkPayload& chunk,
    unsigned long long currentRequestId,
    const VirtualPath& currentFolder,
    bool currentShowNavItems)
{
    return chunk.requestId == currentRequestId &&
        PathEqualsInsensitive(chunk.folder, currentFolder) &&
        chunk.showNavItems == currentShowNavItems;
}

bool ImageBrowserAsyncThumbLoader::ApplyChunkToProgressive(
    const AsyncThumbListChunkPayload& chunk,
    std::vector<VirtualFileEntry>& progressiveListedEntries,
    bool& progressiveUiDirty,
    bool& progressiveLoadCompleted)
{
    bool changed = false;
    if (!chunk.batch.empty())
    {
        progressiveListedEntries.insert(
            progressiveListedEntries.end(),
            chunk.batch.begin(),
            chunk.batch.end());
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
