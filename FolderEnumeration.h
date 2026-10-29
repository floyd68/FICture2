#pragma once

#include "ImageBrowserAsyncThumbLoader.h"
#include "ImageSourceIdentity.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <Windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

// Folder enumeration for the thumbnail strip, owned instead of detached.
//
// A FolderEnumerationService runs enumerations on a bounded worker pool. Each
// browser owns a FolderLoadLane: at most one running and one pending request
// (latest wins); results carry the request id (generation) and reach the lane's
// queue only while the lane is open and the request is current. The browser is
// woken by one payload-free message per batch (FolderListingWakeup) and drains
// its lane; the results never travel in the message.
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
//
// The host runs this as one stage of its post-UI cleanup (RunAppShutdown in
// AppShutdown.h), on normal exit and on the startup failure paths alike:
//   1. IPC server stop (placeholder until the IPC server can be stopped);
//   2. folder enumeration: FolderEnumerationService::Shared().Shutdown(2 s);
//   3. ImageCore: binding invalidation, then the bounded, non-constructing
//      ImageLoader::ShutdownIfCreated(2 s), which requests stop on every
//      request token (also releasing ImageCore threads that wait for a shared
//      Floar archive open) and joins the decode and prefetch threads;
//   4. Floar shared-open service: ArchiveReaderCache::Shared().Shutdown(2 s),
//      which joins the archive open workers (none start if no open ran);
//   5. GPU cache clear, FD2D shutdown, log shutdown, OLE uninitialize.
// Enumeration and ImageCore both read through the archive cache, so both stop
// before it. Each bounded stage makes the same decision: normal when its
// workers joined within the bound, otherwise the forced path through
// EndProcessAfterShutdownTimeout() with that stage's exit code below, before
// any later stage, dependency teardown or static destruction runs. The
// cleanup never persists the session or the window placement (the window's
// before-destroy callback does, once startup handed off).

class FolderEnumerationBackend
{
public:
    // A batch of entries, the source identity of each (same order; empty when
    // the backend captured none, which leaves every identity unknown), and
    // whether the listing is complete.
    using ChunkSink = std::function<void(
        std::vector<Floar::VirtualFileEntry>&&, std::vector<ImageSourceIdentity>&&, bool completed)>;

    virtual ~FolderEnumerationBackend() = default;

    // Enumerates `folder` on a worker thread, reporting entries in batches; the
    // last call has completed == true. Returns early (without a completed batch)
    // once `stop` is requested.
    virtual void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) = 0;
};

// Production backend: the filesystem and Floar archives. Stop is checked
// before every directory operation, so no new (possibly blocking) call starts
// once stop is requested; archive listings stop between entries, and an
// archive scan in progress stops between headers (Floar EnumerateDirectory).
//
// Source identities are captured while listing: a disk file's size and write
// time come from the directory enumeration data (no extra call, except for a
// reparse point, which is read once), and every entry of an archive listing
// gets the host archive's identity, read once per listing.
class FilesystemFolderEnumerationBackend : public FolderEnumerationBackend
{
public:
    void Enumerate(const Floar::VirtualPath& folder, std::stop_token stop, const ChunkSink& sink) override;

protected:
    // The directory operations, one call each (overridden by tests to observe them).
    virtual bool IsDirectory(const std::filesystem::path& path);
    virtual void OpenDirectory(const std::filesystem::path& path, std::filesystem::directory_iterator& it, std::error_code& ec);
    virtual void Advance(std::filesystem::directory_iterator& it, std::error_code& ec);
    virtual bool EntryIsDirectory(const std::filesystem::directory_entry& entry, std::error_code& ec);
    // Size and last-write time of a listed file (unknown when unreadable).
    virtual ImageSourceIdentity EntryIdentity(const std::filesystem::directory_entry& entry);
    // Streams an archive (folder) listing; false once stopped.
    virtual bool EnumerateArchive(
        const Floar::VirtualPath& folder,
        std::stop_token stop,
        const std::function<void(Floar::VirtualFileEntry&&)>& onEntry);
};

// Message (no payload) that wakes a window when folder-listing chunks arrive;
// each image browser then drains its own lane (CMD_FIC2_ASYNC_THUMB_READY).
constexpr UINT kFolderListingReadyMessage = WM_APP + 0x7A13;

// Lifetime-safe wakeup for one window. The window disconnects it when it is
// destroyed; once Disconnect() returns, nothing is posted to that HWND again.
// The message carries no payload, so one discarded with the window leaks
// nothing: the results stay in the lanes.
class FolderListingWakeup
{
public:
    explicit FolderListingWakeup(HWND window);

    // Posts kFolderListingReadyMessage; false when disconnected or posting failed.
    bool Post();
    void Disconnect();

private:
    std::mutex m_mutex {};
    HWND m_window { nullptr };
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

    // Wakes the browser's window when chunks arrive: one message per batch
    // until the queue is drained (null: no wakeups).
    void SetWakeup(std::shared_ptr<FolderListingWakeup> wakeup);

    // Makes `request` current: stops the running enumeration (it keeps its
    // worker until the backend returns) and replaces any pending request.
    void Start(FolderLoadRequest request);

    // Queues a chunk for the browser if the lane is open and the chunk's request
    // is current (workers deliver through this); otherwise drops it.
    void Post(AsyncThumbListChunkPayload&& chunk);

    std::deque<AsyncThumbListChunkPayload> Dequeue();
    bool HasChunks() const;

    // Detaches the lane: stops the running enumeration, drops the pending
    // request and queued chunks, and stops wakeups (none is posted after this
    // returns). Never waits.
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

// Exit codes of the forced shutdown path, one per bounded shutdown stage.
constexpr UINT kFolderEnumerationForcedExitCode = 0xF1C2;   // folder enumeration workers
constexpr UINT kImageCoreForcedExitCode = 0xF1C3;           // ImageCore decode/prefetch threads
constexpr UINT kArchiveOpenForcedExitCode = 0xF1C4;         // Floar archive open workers
constexpr UINT kIpcServerForcedExitCode = 0xF1C5;           // IPC server accept/client threads

// The forced shutdown path shared by every bounded stage: logs that `stage` is
// still running `bound` after its shutdown began, runs `beforeForcedExit`
// (persist state; may be empty), flushes the log and ends the process with
// `exitCode` through TerminateProcess, so no static destructor runs while the
// stage's worker is still alive. Does not return.
void EndProcessAfterShutdownTimeout(
    const char* stage,
    std::chrono::milliseconds bound,
    UINT exitCode,
    const std::function<void()>& beforeForcedExit);

// Normal shutdown when every worker joins within `bound`; otherwise the forced
// path: logs, runs `beforeForcedExit` (persist state), flushes the log and ends
// the process with kFolderEnumerationForcedExitCode. Returns only on success.
void FinishFolderEnumerationOrExit(
    FolderEnumerationService& service,
    std::chrono::milliseconds bound,
    const std::function<void()>& beforeForcedExit);
