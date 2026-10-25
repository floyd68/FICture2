#pragma once

#include "ImageBrowserAsyncThumbLoader.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <Windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

// Folder enumeration for the thumbnail strip, owned instead of detached.
//
// A FolderEnumerationService runs enumerations on a bounded worker pool. Each
// browser owns a FolderLoadLane: at most one running and one pending request
// (latest wins); results carry the request id (generation) and reach the lane's
// queue only while the lane is open and the request is current, after which the
// browser is told by one posted message per batch.
//
// Navigation and browser destruction never wait for a worker: they request stop
// (cooperative, plus best-effort CancelSynchronousIo) and detach the lane. A
// blocked filesystem, network or archive call keeps its worker busy until it
// returns; while every worker is busy, new requests wait for a slot instead of
// spawning threads.
//
// Application shutdown:
//  - Normal path: Shutdown(bound) requests stop on every session, cancels
//    synchronous I/O and joins every worker within the bound; static teardown
//    runs only after that.
//  - Forced path: when a worker is still running after the bound, it is not
//    detached. FinishFolderEnumerationOrExit() saves what must persist (the
//    caller's callback), flushes the log and ends the process with
//    kFolderEnumerationForcedExitCode without running static destructors, so
//    no destructor races a worker that still uses process-wide state.

class FolderEnumerationBackend
{
public:
    using ChunkSink = std::function<void(std::vector<Floar::VirtualFileEntry>&&, bool completed)>;

    virtual ~FolderEnumerationBackend() = default;

    // Enumerates `folder` on a worker thread, reporting entries in batches; the
    // last call has completed == true. Returns early (without a completed batch)
    // once `stop` is requested.
    virtual void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) = 0;
};

// Production backend: the filesystem (checking stop between entries) and
// Floar archives (one snapshot listing).
class FilesystemFolderEnumerationBackend final : public FolderEnumerationBackend
{
public:
    void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) override;
};

struct FolderLoadRequest
{
    unsigned long long requestId { 0 };
    Floar::VirtualPath folder {};
    Floar::VirtualPath preferSelectPath {};
    bool showNavItems { true };
};

namespace FolderEnumerationDetail
{
    struct ServiceState;
    struct LaneState;
}

class FolderLoadLane
{
public:
    FolderLoadLane() = default;
    FolderLoadLane(const FolderLoadLane&) = delete;
    FolderLoadLane& operator=(const FolderLoadLane&) = delete;
    FolderLoadLane(FolderLoadLane&&) noexcept = default;
    FolderLoadLane& operator=(FolderLoadLane&& other) noexcept;
    ~FolderLoadLane();

    explicit operator bool() const { return m_lane != nullptr; }

    // Window that receives CMD_FIC2_ASYNC_THUMB_READY (as an FD2D broadcast)
    // when chunks arrive; one message per batch until the queue is drained.
    void SetNotifyWindow(HWND window);

    // Makes `request` current: stops the running enumeration (it keeps its
    // worker until the backend returns) and replaces any pending request.
    void Start(FolderLoadRequest request);

    // Queues a chunk for the browser if the lane is open and the chunk's request
    // is current (workers deliver through this); otherwise drops it.
    void Post(AsyncThumbListChunkPayload&& chunk);

    std::deque<AsyncThumbListChunkPayload> Dequeue();
    bool HasChunks() const;

    // Detaches the lane: stops the running enumeration, drops the pending
    // request and queued chunks, and stops notifications. Never waits.
    void Close();

private:
    friend class FolderEnumerationService;
    FolderLoadLane(
        std::shared_ptr<FolderEnumerationDetail::ServiceState> service,
        std::shared_ptr<FolderEnumerationDetail::LaneState> lane);

    std::shared_ptr<FolderEnumerationDetail::ServiceState> m_service {};
    std::shared_ptr<FolderEnumerationDetail::LaneState> m_lane {};
};

class FolderEnumerationService
{
public:
    static constexpr size_t kDefaultMaxWorkers = 4;

    FolderEnumerationService(std::shared_ptr<FolderEnumerationBackend> backend, size_t maxWorkers);
    FolderEnumerationService(const FolderEnumerationService&) = delete;
    FolderEnumerationService& operator=(const FolderEnumerationService&) = delete;
    // Shuts down without a bound (joins every worker) unless Shutdown succeeded.
    ~FolderEnumerationService();

    // The application's service (filesystem backend, default cap).
    static FolderEnumerationService& Shared();

    FolderLoadLane CreateLane();

    // Stops every session, drops pending requests, and joins all workers if they
    // finish within `bound`. Returns false (workers left running, not detached)
    // otherwise. Later requests are ignored.
    bool Shutdown(std::chrono::milliseconds bound);

    // Live worker threads (never more than the cap) and running enumerations.
    size_t WorkerCount() const;
    size_t RunningCount() const;

private:
    std::shared_ptr<FolderEnumerationDetail::ServiceState> m_state {};
};

// Exit code of the forced shutdown path.
constexpr UINT kFolderEnumerationForcedExitCode = 0xF1C2;

// Normal shutdown when every worker joins within `bound`; otherwise the forced
// path: logs, runs `beforeForcedExit` (persist state), flushes the log and ends
// the process with kFolderEnumerationForcedExitCode. Returns only on success.
void FinishFolderEnumerationOrExit(
    FolderEnumerationService& service,
    std::chrono::milliseconds bound,
    const std::function<void()>& beforeForcedExit);
