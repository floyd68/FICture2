#include "CloudThumbnailRouter.h"

#include "AppLog.h"
#include "ImageCore/ImageLoader.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace
{
    // ImageCore::ImageLoader as a decode service (the inner service of the
    // production router).
    class ImageLoaderDecodeService final : public IImageDecodeService
    {
    public:
        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback,
            ImageCore::RequestPriority priority) override
        {
            return ImageCore::ImageLoader::Instance().RequestDecoded(request, std::move(callback), priority);
        }

        void Cancel(ImageCore::ImageHandle handle) override
        {
            ImageCore::ImageLoader::Instance().Cancel(handle);
        }

        void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) override
        {
            ImageCore::ImageLoader::Instance().SetPriority(handle, priority);
        }
    };

    std::atomic<CloudThumbnailRouter*> g_sharedRouter { nullptr };

    // The shell call's size: the request box, rounded up (a fixed default
    // when the request has none).
    uint32_t ShellSide(float side)
    {
        constexpr uint32_t kDefaultSide = 256;
        constexpr uint32_t kMaxSide = 2560;
        if (!(side > 0.0f))
        {
            return kDefaultSide;
        }
        return (std::min)(kMaxSide, (std::max)(1u, static_cast<uint32_t>(std::ceil(side))));
    }

    // Dispatches the messages queued for this thread: the workers are STAs,
    // and a provider may complete pending work through them.
    void PumpMessages()
    {
        MSG msg {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message != WM_QUIT)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    // Waits until `wake` is signaled, a message arrives or `timeout` passes,
    // then dispatches the queued messages.
    void WaitAndPumpMessages(HANDLE wake, DWORD timeout)
    {
        (void)MsgWaitForMultipleObjectsEx(1, &wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        PumpMessages();
    }

    // Milliseconds from `now` until `when` (INFINITE for the max time point).
    DWORD TimeoutUntil(std::chrono::steady_clock::time_point when, std::chrono::steady_clock::time_point now)
    {
        if (when == std::chrono::steady_clock::time_point::max())
        {
            return INFINITE;
        }
        if (when <= now)
        {
            return 0;
        }
        const long long ms = std::chrono::ceil<std::chrono::milliseconds>(when - now).count();
        return static_cast<DWORD>((std::min)(ms, static_cast<long long>(INFINITE - 1)));
    }
}

CloudThumbnailRouter::CloudThumbnailRouter(
    IImageDecodeService& inner,
    IShellThumbnailSource& shell,
    const PathClassificationQueries* queries,
    Options options)
    : m_inner(inner)
    , m_shell(shell)
    , m_classifier(queries)
    , m_options(options)
{
    m_options.workers = (std::max)(size_t { 1 }, m_options.workers);
}

CloudThumbnailRouter::~CloudThumbnailRouter()
{
    if (!Shutdown(std::chrono::hours(24)))
    {
        // A worker that has not exited by now keeps its thread and events.
        for (std::thread& worker : m_workers)
        {
            worker.detach();
        }
        return;
    }
    for (HANDLE wake : m_wakeEvents)
    {
        CloseHandle(wake);
    }
}

bool CloudThumbnailRouter::RoutesToShell(const ImageCore::ImageRequest& request) const
{
    return request.purpose == ImageCore::ImagePurpose::Thumbnail &&
        m_classifier.Classify(request.source) == HostPathKind::CloudPlaceholder;
}

ImageCore::ImageHandle CloudThumbnailRouter::RequestDecoded(
    const ImageCore::ImageRequest& request,
    ImageCore::DecodedImageLoadCallback callback,
    ImageCore::RequestPriority priority)
{
    if (!RoutesToShell(request))
    {
        return m_inner.RequestDecoded(request, std::move(callback), priority);
    }

    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        if (!m_shutDown)
        {
            auto job = std::make_shared<Job>();
            handle = kShellHandleBit | ++m_nextId;
            job->handle = handle;
            job->request = request;
            job->callback = std::move(callback);
            job->priority = priority;
            job->sequence = ++m_nextSequence;
            job->due = std::chrono::steady_clock::now();
            m_core->jobs.emplace(handle, std::move(job));
            ++m_stats.routed;
            StartWorkers_NoLock();
            WakeWorkers_NoLock();
        }
    }
    if (handle == 0)
    {
        // Shut down: everything passes through.
        return m_inner.RequestDecoded(request, std::move(callback), priority);
    }
    return handle;
}

void CloudThumbnailRouter::Cancel(ImageCore::ImageHandle handle)
{
    if ((handle & kShellHandleBit) == 0)
    {
        m_inner.Cancel(handle);
        return;
    }

    // Never waits for a fallback's admission (the inner service may block in
    // it): a fallback not admitted yet sees the cancel and never starts, and
    // one being admitted cancels its inner request when the admission returns.
    ImageCore::ImageHandle innerHandle = 0;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        const auto it = m_core->jobs.find(handle);
        if (it == m_core->jobs.end())
        {
            return;
        }
        const std::shared_ptr<Job> job = it->second;
        m_core->jobs.erase(it);
        job->canceled = true;
        ++m_stats.canceled;
        innerHandle = job->innerHandle;   // 0 unless admitted
    }
    if (innerHandle != 0)
    {
        m_inner.Cancel(innerHandle);
    }
}

void CloudThumbnailRouter::SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority)
{
    if ((handle & kShellHandleBit) == 0)
    {
        m_inner.SetPriority(handle, priority);
        return;
    }

    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        const auto it = m_core->jobs.find(handle);
        if (it == m_core->jobs.end())
        {
            return;
        }
        job = it->second;
        job->priority = priority;
    }
    // Not admitted yet: FallBack dispatches at the latest priority.
    ForwardPriority(job);
}

void CloudThumbnailRouter::ForwardPriority(const std::shared_ptr<Job>& job)
{
    // One forwarder per job at a time, and it sends until the inner request
    // has the latest priority; any other caller returns without waiting, its
    // update picked up by the running forwarder. So the last priority the
    // inner service receives is the last one set.
    std::unique_lock<std::mutex> lock(m_core->mutex);
    if (job->innerHandle == 0 || job->forwarding)
    {
        return;
    }
    job->forwarding = true;
    while (!job->canceled && job->priority != job->sentPriority)
    {
        const ImageCore::RequestPriority latest = job->priority;
        job->sentPriority = latest;
        const ImageCore::ImageHandle innerHandle = job->innerHandle;
        lock.unlock();
        m_inner.SetPriority(innerHandle, latest);
        lock.lock();
    }
    job->forwarding = false;
}

void CloudThumbnailRouter::StartWorkers_NoLock()
{
    if (!m_workers.empty() || m_stopping)
    {
        return;
    }
    for (size_t i = 0; i < m_options.workers; ++i)
    {
        m_wakeEvents.push_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }
    for (HANDLE wake : m_wakeEvents)
    {
        m_workers.emplace_back([this, wake] { WorkerMain(wake); });
    }
}

void CloudThumbnailRouter::WakeWorkers_NoLock()
{
    for (HANDLE wake : m_wakeEvents)
    {
        SetEvent(wake);
    }
}

std::shared_ptr<CloudThumbnailRouter::Job> CloudThumbnailRouter::PickDue_NoLock(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point& wakeAt) const
{
    wakeAt = std::chrono::steady_clock::time_point::max();
    std::shared_ptr<Job> best;
    for (const auto& [handle, job] : m_core->jobs)
    {
        if (job->state != JobState::Queued)
        {
            continue;
        }
        if (job->due > now)
        {
            wakeAt = (std::min)(wakeAt, job->due);
            continue;
        }
        if (!best || job->priority > best->priority ||
            (job->priority == best->priority && job->sequence < best->sequence))
        {
            best = job;
        }
    }
    return best;
}

void CloudThumbnailRouter::WorkerMain(HANDLE wake)
{
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::unique_lock<std::mutex> lock(m_core->mutex);
    while (!m_stopping)
    {
        // The apartment's messages are dispatched between calls, also while
        // there is always work queued.
        lock.unlock();
        PumpMessages();
        lock.lock();
        if (m_stopping)
        {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point wakeAt {};
        const std::shared_ptr<Job> job = PickDue_NoLock(now, wakeAt);
        if (!job)
        {
            lock.unlock();
            WaitAndPumpMessages(wake, TimeoutUntil(wakeAt, now));
            lock.lock();
            continue;
        }

        job->state = JobState::Running;
        if (!job->called)
        {
            job->called = true;
            job->firstCall = now;
        }
        ++m_stats.shellCalls;
        const std::wstring path = job->request.source;
        const uint32_t width = ShellSide(job->request.targetSize.w);
        const uint32_t height = ShellSide(job->request.targetSize.h);
        lock.unlock();

        ShellThumbnailResult result = m_shell.GetThumbnail(path, width, height);

        lock.lock();
        if (job->canceled || m_stopping)
        {
            continue;   // removed by Cancel or Shutdown: publish nothing
        }
        now = std::chrono::steady_clock::now();
        if (result.status == ShellThumbnailStatus::Ready && result.image.blocks && !result.image.blocks->empty() &&
            result.image.width > 0 && result.image.height > 0)
        {
            m_core->jobs.erase(job->handle);
            ++m_stats.ready;
            lock.unlock();
            job->callback(S_OK, std::move(result.image));
            lock.lock();
            continue;
        }
        if (result.status == ShellThumbnailStatus::Pending && now - job->firstCall < m_options.pendingDeadline)
        {
            // The provider is fetching the thumbnail: ask again later, behind
            // the requests queued meanwhile at the same priority.
            const uint32_t shift = (std::min)(job->polls, 16u);
            const auto delay = (std::min)(m_options.maxPoll, m_options.firstPoll * (int64_t { 1 } << shift));
            ++job->polls;
            job->state = JobState::Queued;
            job->due = now + delay;
            job->sequence = ++m_nextSequence;
            continue;
        }

        job->state = JobState::Fallback;
        ++m_stats.fallbacks;
        lock.unlock();
        FIC2_LOG_INFO("[CloudThumb] No shell thumbnail ({}, hr=0x{:08X}); reading the file instead.",
            result.status == ShellThumbnailStatus::Pending ? "still pending" : "failed",
            static_cast<uint32_t>(result.hr));
        FallBack(job);
        lock.lock();
    }

    lock.unlock();
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
}

void CloudThumbnailRouter::FallBack(const std::shared_ptr<Job>& job)
{
    if (m_options.beforeFallbackForTesting)
    {
        m_options.beforeFallbackForTesting();
    }

    ImageCore::RequestPriority priority {};
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        if (job->canceled)
        {
            return;   // canceled before admission: no read starts
        }
        priority = job->priority;
        job->sentPriority = priority;
    }

    // The completion leaves the router's table and is forwarded unless the
    // request was canceled meanwhile.
    std::weak_ptr<Core> weakCore = m_core;
    auto forward = [weakCore, job](HRESULT hr, ImageCore::DecodedImage image)
    {
        const std::shared_ptr<Core> core = weakCore.lock();
        if (!core)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            if (job->canceled)
            {
                return;
            }
            const auto it = core->jobs.find(job->handle);
            if (it != core->jobs.end() && it->second == job)
            {
                core->jobs.erase(it);
            }
        }
        job->callback(hr, std::move(image));
    };
    const ImageCore::ImageHandle innerHandle = m_inner.RequestDecoded(job->request, std::move(forward), priority);

    // A Cancel or SetPriority made during the dispatch only recorded itself
    // (innerHandle was 0); apply it now.
    bool cancelInner = false;
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        if (job->canceled)
        {
            cancelInner = innerHandle != 0;
        }
        else if (innerHandle == 0)
        {
            m_core->jobs.erase(job->handle);
            rejected = true;
        }
        else
        {
            job->innerHandle = innerHandle;
        }
    }
    if (cancelInner)
    {
        m_inner.Cancel(innerHandle);
    }
    else if (!rejected)
    {
        ForwardPriority(job);
    }
    if (rejected)
    {
        // The inner service rejected it (for example after its shutdown) and
        // will never call back.
        job->callback(E_ABORT, {});
    }
}

bool CloudThumbnailRouter::Shutdown(std::chrono::milliseconds bound)
{
    const auto deadline = std::chrono::steady_clock::now() + bound;
    std::vector<ImageCore::ImageHandle> innerHandles;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_stopping = true;
        m_shutDown = true;
        for (const auto& [handle, job] : m_core->jobs)
        {
            job->canceled = true;
            if (job->state == JobState::Fallback && job->innerHandle != 0)
            {
                innerHandles.push_back(job->innerHandle);
            }
        }
        m_core->jobs.clear();
        WakeWorkers_NoLock();
    }
    for (const ImageCore::ImageHandle handle : innerHandles)
    {
        m_inner.Cancel(handle);
    }

    // One caller at a time owns the workers while it waits for them, so a
    // concurrent caller never sees them missing and reports success early; it
    // waits for the owner within its own bound instead.
    std::unique_lock<std::timed_mutex> lifecycle(m_lifecycle, std::defer_lock);
    if (!lifecycle.try_lock_until(deadline))
    {
        return false;
    }

    // A worker is done only when its thread has terminated (after
    // CoUninitialize and its thread-local destructors), so the bound waits
    // on the native threads; a join before that could block past it.
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        workers = std::move(m_workers);
        m_workers.clear();
    }
    std::vector<std::thread> running;
    for (std::thread& worker : workers)
    {
        const DWORD timeout = TimeoutUntil(deadline, std::chrono::steady_clock::now());
        if (WaitForSingleObject(static_cast<HANDLE>(worker.native_handle()), timeout) == WAIT_OBJECT_0)
        {
            worker.join();
        }
        else
        {
            running.push_back(std::move(worker));
        }
    }
    if (running.empty())
    {
        return true;
    }

    // Kept, not detached: a later Shutdown can still join them.
    FIC2_LOG_WARN("[CloudThumb] A shell thumbnail worker has not exited at shutdown.");
    std::lock_guard<std::mutex> lock(m_core->mutex);
    for (std::thread& worker : running)
    {
        m_workers.push_back(std::move(worker));
    }
    return false;
}

CloudThumbnailRouter::Stats CloudThumbnailRouter::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_stats;
}

size_t CloudThumbnailRouter::OutstandingShellRequests() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    size_t count = 0;
    for (const auto& [handle, job] : m_core->jobs)
    {
        count += job->state != JobState::Fallback ? 1 : 0;
    }
    return count;
}

CloudThumbnailRouter& CloudThumbnailRouter::Shared()
{
    // Never destroyed: a worker detached at a timed-out shutdown may still
    // reference it until the process ends.
    static CloudThumbnailRouter* const s_router = []
    {
        static ImageLoaderDecodeService s_imageLoader;
        auto* router = new CloudThumbnailRouter(s_imageLoader, IShellThumbnailSource::System());
        g_sharedRouter.store(router);
        return router;
    }();
    return *s_router;
}

bool CloudThumbnailRouter::ShutdownSharedIfCreated(std::chrono::milliseconds bound)
{
    CloudThumbnailRouter* router = g_sharedRouter.load();
    return router == nullptr || router->Shutdown(bound);
}
