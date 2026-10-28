#pragma once

// CloudThumbnailRouter: thumbnails of online-only cloud files come from the
// shell instead of the file's bytes (#59).
//
// It is the decode service of ImageAsyncBinding (IImageDecodeService) in
// front of ImageCore, so ImageCore is unchanged:
// - Routing: a Thumbnail request whose path HostPathClassifier reports as a
//   cloud placeholder (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS or
//   FILE_ATTRIBUTE_OFFLINE on a fixed drive, the detection ImageCore's lanes
//   use) goes to the shell source. Every other request, including a main
//   image (FullResolution) of an online-only file, which must download, goes
//   to the inner service (ImageCore) as before. The classification runs on
//   the caller's thread, like ImageCore's own (a local metadata query).
// - Workers: a fixed pool (Options::workers, started with the first routed
//   request) runs the shell calls, each thread with COM initialized as STA;
//   never on the caller's (UI) thread, and no thread per request. Higher
//   priority first, then oldest first.
// - Pending: when the provider is still fetching the thumbnail, the request
//   waits (without holding a worker) and is asked again after a backoff from
//   Options::firstPoll up to Options::maxPoll, until Options::pendingDeadline
//   after its first call.
// - Fallback: a shell failure, or a thumbnail still pending at the deadline,
//   sends the request to the inner service, the byte path. That downloads
//   the file, as every thumbnail did before; it is taken only for files the
//   shell has no thumbnail for (for example a type the provider does not
//   thumbnail).
// - Size: the shell fits its thumbnail into the request box and may return a
//   smaller one. The payload is published at its actual size, so the strip's
//   size policy (ThumbPayloadCache::IsSourceLimited, ThumbSizePolicy) records
//   a thumbnail smaller than its box on both sides as source-limited and asks
//   for no larger one; an upgrade request is again a shell request.
// - Cancellation (the #45 contract): Cancel removes a queued or waiting
//   request before its shell call, and drops a running one when its call
//   returns; a canceled request publishes nothing and never falls back. Late
//   results of superseded requests are dropped by ImageAsyncBinding's
//   generation gate as for ImageCore completions.
// - Handles: routed requests get handles with kShellHandleBit set; others are
//   the inner service's handles, passed through. Cancel and SetPriority of a
//   request that fell back reach its inner handle.
// - Shutdown stops the workers (bounded) and drops every request without a
//   completion; later requests all pass through to the inner service.

#include "FloarPathByteSource.h"
#include "ImageAsyncBinding.h"
#include "ShellThumbnailSource.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class CloudThumbnailRouter final : public IImageDecodeService
{
public:
    static constexpr ImageCore::ImageHandle kShellHandleBit = ImageCore::ImageHandle { 1 } << 63;

    struct Options
    {
        size_t workers { 2 };
        std::chrono::milliseconds firstPoll { 50 };
        std::chrono::milliseconds maxPoll { 400 };
        std::chrono::milliseconds pendingDeadline { 15000 };
    };

    struct Stats
    {
        size_t routed { 0 };       // requests sent to the shell source
        size_t shellCalls { 0 };   // GetThumbnail calls (a pending request counts each)
        size_t ready { 0 };        // published shell thumbnails
        size_t fallbacks { 0 };    // requests sent to the byte path after the shell
        size_t canceled { 0 };     // routed requests canceled before publishing
    };

    // `inner`, `shell` and `queries` (null: the system queries) must outlive
    // the router.
    CloudThumbnailRouter(
        IImageDecodeService& inner,
        IShellThumbnailSource& shell,
        const PathClassificationQueries* queries = nullptr,
        Options options = {});
    ~CloudThumbnailRouter() override;

    CloudThumbnailRouter(const CloudThumbnailRouter&) = delete;
    CloudThumbnailRouter& operator=(const CloudThumbnailRouter&) = delete;

    ImageCore::ImageHandle RequestDecoded(
        const ImageCore::ImageRequest& request,
        ImageCore::DecodedImageLoadCallback callback,
        ImageCore::RequestPriority priority) override;
    void Cancel(ImageCore::ImageHandle handle) override;
    void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) override;

    // Whether `request` goes to the shell source.
    bool RoutesToShell(const ImageCore::ImageRequest& request) const;

    // Stops routing, drops every routed request without a completion
    // (canceling inner handles of those that fell back) and joins the workers
    // within `bound`. False when a worker is still in a shell call at the
    // bound (it is detached and publishes nothing).
    bool Shutdown(std::chrono::milliseconds bound);

    Stats GetStats() const;
    // Routed requests not yet published, canceled or fallen back.
    size_t OutstandingShellRequests() const;

    // The production router: ImageCore::ImageLoader behind it, the system
    // shell source and queries. Created on first use.
    static CloudThumbnailRouter& Shared();
    // Shuts down the production router if it was created; true otherwise.
    static bool ShutdownSharedIfCreated(std::chrono::milliseconds bound);

private:
    enum class JobState
    {
        Queued,     // waiting for a worker (or for its next poll)
        Running,    // in a shell call
        Fallback,   // sent to the inner service
    };

    struct Job
    {
        ImageCore::ImageHandle handle { 0 };
        ImageCore::ImageRequest request {};
        ImageCore::DecodedImageLoadCallback callback {};
        ImageCore::RequestPriority priority { ImageCore::RequestPriority::Visible };
        uint64_t sequence { 0 };   // queue order within a priority
        JobState state { JobState::Queued };
        std::chrono::steady_clock::time_point due {};
        std::chrono::steady_clock::time_point firstCall {};
        bool called { false };
        uint32_t polls { 0 };
        bool canceled { false };   // removed while running or falling back
        ImageCore::ImageHandle innerHandle { 0 };
    };

    void StartWorkers_NoLock();
    void WorkerMain();
    // The due queued job to run next, or null (`wakeAt` then holds the
    // earliest due time of a waiting job, or max when none waits).
    std::shared_ptr<Job> PickDue_NoLock(std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point& wakeAt) const;
    // Sends `job` to the inner service; called without the lock.
    void FallBack(const std::shared_ptr<Job>& job);

    // The routed requests by handle, shared with the completion forwarders of
    // requests that fell back (which may outlive the router).
    struct Core
    {
        std::mutex mutex;
        std::unordered_map<ImageCore::ImageHandle, std::shared_ptr<Job>> jobs;
    };

    IImageDecodeService& m_inner;
    IShellThumbnailSource& m_shell;
    HostPathClassifier m_classifier;
    Options m_options;

    // Everything below is guarded by m_core->mutex.
    const std::shared_ptr<Core> m_core { std::make_shared<Core>() };
    std::condition_variable m_wake;
    std::condition_variable m_exited;
    uint64_t m_nextId { 0 };
    uint64_t m_nextSequence { 0 };
    bool m_stopping { false };
    bool m_shutDown { false };
    std::vector<std::thread> m_workers;
    size_t m_liveWorkers { 0 };
    Stats m_stats {};
};
