#include "FolderEnumeration.h"
#include "AppLog.h"
#include "FD2D/Backplate.h"

#include <condition_variable>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <thread>

namespace
{
    // Must match ImageBrowserImpl::CMD_FIC2_ASYNC_THUMB_READY.
    constexpr UINT kAsyncThumbReadyCommand = WM_APP + 0x7A13;
}

namespace FolderEnumerationDetail
{
    struct Worker
    {
        std::thread thread {};
        HANDLE handle { nullptr };
        // Lane whose request this worker is running (null while idle).
        std::shared_ptr<LaneState> lane {};
    };

    // Every field is guarded by ServiceState::mutex.
    struct LaneState
    {
        bool closed { false };
        unsigned long long currentRequestId { 0 };
        std::optional<FolderLoadRequest> pending {};
        bool queued { false };       // in ServiceState::runnable
        Worker* worker { nullptr };  // running this lane's request, if any
        std::stop_source stop {};    // of the running request

        std::deque<AsyncThumbListChunkPayload> chunks {};
        HWND window { nullptr };
        bool notifyPending { false };  // posted, not yet drained
    };

    struct ServiceState
    {
        std::shared_ptr<FolderEnumerationBackend> backend {};
        size_t maxWorkers { 1 };

        mutable std::mutex mutex {};
        std::condition_variable workAvailable {};
        std::condition_variable workerExited {};
        std::deque<std::shared_ptr<LaneState>> runnable {};
        std::list<Worker> workers {};  // stable addresses; joined only by Shutdown
        size_t liveWorkers { 0 };
        size_t idleWorkers { 0 };
        size_t running { 0 };
        bool shutdown { false };
    };

    namespace
    {
        void RequestStopLocked(LaneState& lane)
        {
            lane.stop.request_stop();
            if (lane.worker != nullptr && lane.worker->handle != nullptr)
            {
                // Best effort: not every (network) directory call honours it.
                (void)CancelSynchronousIo(lane.worker->handle);
            }
        }

        void PostChunk(ServiceState& service, LaneState& lane, AsyncThumbListChunkPayload&& chunk)
        {
            HWND notifyWindow = nullptr;
            {
                std::lock_guard<std::mutex> lock(service.mutex);
                if (lane.closed || chunk.requestId != lane.currentRequestId)
                {
                    return;
                }
                lane.chunks.push_back(std::move(chunk));
                if (lane.window != nullptr && !lane.notifyPending)
                {
                    lane.notifyPending = true;
                    notifyWindow = lane.window;
                }
            }

            // The UI drains the queue when this message arrives (FD2D broadcast ->
            // ImageBrowser command handler).
            if (notifyWindow != nullptr)
            {
                auto* message = new FD2D::Backplate::BroadcastMessage();
                message->message = kAsyncThumbReadyCommand;
                if (!PostMessageW(notifyWindow, FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(message)))
                {
                    delete message;
                    std::lock_guard<std::mutex> lock(service.mutex);
                    lane.notifyPending = false;
                }
            }
        }

        void RunRequest(
            ServiceState& service,
            const std::shared_ptr<LaneState>& lane,
            const FolderLoadRequest& request,
            std::stop_token stop)
        {
            const auto sink = [&](std::vector<Floar::VirtualFileEntry>&& batch, bool completed)
            {
                if (stop.stop_requested())
                {
                    return;  // superseded or detached
                }
                AsyncThumbListChunkPayload chunk {};
                chunk.requestId = request.requestId;
                chunk.folder = request.folder;
                chunk.preferSelectPath = request.preferSelectPath;
                chunk.showNavItems = request.showNavItems;
                chunk.completed = completed;
                chunk.batch = std::move(batch);
                PostChunk(service, *lane, std::move(chunk));
            };

            try
            {
                service.backend->Enumerate(request.folder, stop, sink);
            }
            catch (const std::exception& ex)
            {
                FIC2_LOG_WARN("[FolderEnum] Enumeration of '{}' failed: {}", AppLog::ToUtf8(request.folder.wstring()), ex.what());
                sink({}, true);
            }
            catch (...)
            {
                FIC2_LOG_WARN("[FolderEnum] Enumeration of '{}' failed.", AppLog::ToUtf8(request.folder.wstring()));
                sink({}, true);
            }
        }

        void WorkerMain(std::shared_ptr<ServiceState> service, Worker* self)
        {
            std::unique_lock<std::mutex> lock(service->mutex);
            for (;;)
            {
                ++service->idleWorkers;
                service->workAvailable.wait(lock, [&] { return service->shutdown || !service->runnable.empty(); });
                --service->idleWorkers;
                if (service->shutdown)
                {
                    break;
                }

                std::shared_ptr<LaneState> lane = std::move(service->runnable.front());
                service->runnable.pop_front();
                lane->queued = false;
                if (lane->closed || !lane->pending)
                {
                    continue;
                }

                const FolderLoadRequest request = std::move(*lane->pending);
                lane->pending.reset();
                lane->stop = std::stop_source {};
                lane->worker = self;
                self->lane = lane;
                ++service->running;
                const std::stop_token stop = lane->stop.get_token();

                lock.unlock();
                RunRequest(*service, lane, request, stop);
                lock.lock();

                --service->running;
                self->lane.reset();
                lane->worker = nullptr;
                // A request that arrived while this one ran waits for this lane.
                if (!lane->closed && lane->pending && !service->shutdown && !lane->queued)
                {
                    lane->queued = true;
                    service->runnable.push_back(lane);
                }
            }

            --service->liveWorkers;
            service->workerExited.notify_all();
        }

        // A worker is added only when queued work exceeds the idle workers and
        // the cap allows it; otherwise the request waits for a slot.
        void SpawnWorkerIfNeededLocked(const std::shared_ptr<ServiceState>& service)
        {
            if (service->runnable.size() <= service->idleWorkers || service->workers.size() >= service->maxWorkers)
            {
                return;
            }
            Worker& worker = service->workers.emplace_back();
            ++service->liveWorkers;
            // The worker waits for the lock held here before touching `worker`.
            worker.thread = std::thread(WorkerMain, service, &worker);
            worker.handle = worker.thread.native_handle();
        }
    }
}

using namespace FolderEnumerationDetail;

void FilesystemFolderEnumerationBackend::Enumerate(
    const Floar::VirtualPath& folder,
    std::stop_token stop,
    const ChunkSink& sink)
{
    if (folder.IsInArchive() || folder.IsArchiveFile())
    {
        // Archive listing is one snapshot (not cancellable before it returns).
        std::vector<Floar::VirtualFileEntry> entries = Floar::VirtualFileSystem::ListDirectory(folder);
        if (!stop.stop_requested())
        {
            sink(std::move(entries), true);
        }
        return;
    }

    // Same listing as Floar::VirtualFileSystem::EnumerateFilesystemDirectory,
    // but checking the stop request between entries.
    std::error_code ec;
    if (!std::filesystem::is_directory(folder.hostPath, ec))
    {
        sink({}, true);
        return;
    }
    std::filesystem::directory_iterator it(folder.hostPath, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        sink({}, true);
        return;
    }

    // Fewer, larger chunks reduce progressive UI rebuild overhead on large folders.
    constexpr size_t kBatchSize = 256;
    std::vector<Floar::VirtualFileEntry> batch {};
    batch.reserve(kBatchSize);
    const std::filesystem::directory_iterator end {};
    for (; it != end; it.increment(ec))
    {
        if (stop.stop_requested())
        {
            return;
        }
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
        batch.emplace_back(Floar::VirtualPath(entry.path()), isDir, 0, 0);
        if (batch.size() >= kBatchSize)
        {
            sink(std::move(batch), false);
            batch.clear();
            batch.reserve(kBatchSize);
        }
    }

    if (!stop.stop_requested())
    {
        // Empty (failed / no entries) or the final partial batch either way.
        sink(std::move(batch), true);
    }
}

FolderLoadLane::FolderLoadLane(std::shared_ptr<ServiceState> service, std::shared_ptr<LaneState> lane)
    : m_service(std::move(service))
    , m_lane(std::move(lane))
{
}

FolderLoadLane& FolderLoadLane::operator=(FolderLoadLane&& other) noexcept
{
    if (this != &other)
    {
        Close();
        m_service = std::move(other.m_service);
        m_lane = std::move(other.m_lane);
    }
    return *this;
}

FolderLoadLane::~FolderLoadLane()
{
    Close();
}

void FolderLoadLane::SetNotifyWindow(HWND window)
{
    if (!m_lane)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_service->mutex);
    m_lane->window = m_lane->closed ? nullptr : window;
}

void FolderLoadLane::Start(FolderLoadRequest request)
{
    if (!m_lane)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_service->mutex);
    if (m_lane->closed || m_service->shutdown)
    {
        return;
    }

    m_lane->currentRequestId = request.requestId;
    m_lane->pending = std::move(request);  // replaces an older pending request
    if (m_lane->worker != nullptr)
    {
        // Runs again on this lane once the current enumeration returns.
        RequestStopLocked(*m_lane);
        return;
    }
    if (!m_lane->queued)
    {
        m_lane->queued = true;
        m_service->runnable.push_back(m_lane);
    }
    SpawnWorkerIfNeededLocked(m_service);
    m_service->workAvailable.notify_one();
}

void FolderLoadLane::Post(AsyncThumbListChunkPayload&& chunk)
{
    if (m_lane)
    {
        PostChunk(*m_service, *m_lane, std::move(chunk));
    }
}

std::deque<AsyncThumbListChunkPayload> FolderLoadLane::Dequeue()
{
    std::deque<AsyncThumbListChunkPayload> chunks {};
    if (!m_lane)
    {
        return chunks;
    }
    std::lock_guard<std::mutex> lock(m_service->mutex);
    chunks = std::move(m_lane->chunks);
    m_lane->chunks.clear();
    // Chunks queued after this drain need a new message.
    m_lane->notifyPending = false;
    return chunks;
}

bool FolderLoadLane::HasChunks() const
{
    if (!m_lane)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_service->mutex);
    return !m_lane->chunks.empty();
}

void FolderLoadLane::Close()
{
    if (!m_lane)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_service->mutex);
        LaneState& lane = *m_lane;
        lane.closed = true;
        lane.pending.reset();
        lane.chunks.clear();
        lane.window = nullptr;
        if (lane.worker != nullptr)
        {
            RequestStopLocked(lane);
        }
        if (lane.queued)
        {
            lane.queued = false;
            std::erase(m_service->runnable, m_lane);
        }
    }
    m_lane.reset();
    m_service.reset();
}

FolderEnumerationService::FolderEnumerationService(std::shared_ptr<FolderEnumerationBackend> backend, size_t maxWorkers)
    : m_state(std::make_shared<ServiceState>())
{
    m_state->backend = std::move(backend);
    m_state->maxWorkers = (maxWorkers == 0) ? 1 : maxWorkers;
}

FolderEnumerationService::~FolderEnumerationService()
{
    while (!Shutdown(std::chrono::hours(1)))
    {
    }
}

FolderEnumerationService& FolderEnumerationService::Shared()
{
    static FolderEnumerationService service(std::make_shared<FilesystemFolderEnumerationBackend>(), kDefaultMaxWorkers);
    return service;
}

FolderLoadLane FolderEnumerationService::CreateLane()
{
    return FolderLoadLane(m_state, std::make_shared<LaneState>());
}

bool FolderEnumerationService::Shutdown(std::chrono::milliseconds bound)
{
    std::list<Worker> workers {};
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        if (!m_state->shutdown)
        {
            m_state->shutdown = true;
            for (Worker& worker : m_state->workers)
            {
                if (worker.lane)
                {
                    RequestStopLocked(*worker.lane);
                }
            }
            for (const auto& lane : m_state->runnable)
            {
                lane->queued = false;
                lane->pending.reset();
            }
            m_state->runnable.clear();
            m_state->workAvailable.notify_all();
        }

        if (!m_state->workerExited.wait_for(lock, bound, [&] { return m_state->liveWorkers == 0; }))
        {
            return false;
        }
        workers = std::move(m_state->workers);
        m_state->workers.clear();
    }

    // Every worker has left its loop; join them before any static teardown.
    for (Worker& worker : workers)
    {
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
    }
    return true;
}

size_t FolderEnumerationService::WorkerCount() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->workers.size();
}

size_t FolderEnumerationService::RunningCount() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->running;
}

void FinishFolderEnumerationOrExit(
    FolderEnumerationService& service,
    std::chrono::milliseconds bound,
    const std::function<void()>& beforeForcedExit)
{
    if (service.Shutdown(bound))
    {
        return;
    }

    FIC2_LOG_WARN("[FolderEnum] {} enumeration(s) still running {} ms after shutdown; ending the process without static teardown (exit code {}).",
        service.RunningCount(), bound.count(), kFolderEnumerationForcedExitCode);
    if (beforeForcedExit)
    {
        beforeForcedExit();
    }
    AppLog::Shutdown();
    TerminateProcess(GetCurrentProcess(), kFolderEnumerationForcedExitCode);
}
