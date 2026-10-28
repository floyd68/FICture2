#include "ImageAsyncBinding.h"
#include "AppLog.h"
#include "CommonUtil.h"
#include "ImageCore/DecodedImage.h"

#include <algorithm>
#include <chrono>

namespace
{
    std::atomic<IImageDecodeService*> g_decodeServiceOverride { nullptr };

    ImageCore::ImageHandle RequestDecode(
        const ImageCore::ImageRequest& request,
        ImageCore::DecodedImageLoadCallback callback,
        ImageCore::RequestPriority priority)
    {
        if (IImageDecodeService* service = g_decodeServiceOverride.load())
        {
            return service->RequestDecoded(request, std::move(callback), priority);
        }
        return ImageCore::ImageLoader::Instance().RequestDecoded(request, std::move(callback), priority);
    }

    void SetDecodePriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority)
    {
        if (IImageDecodeService* service = g_decodeServiceOverride.load())
        {
            service->SetPriority(handle, priority);
            return;
        }
        ImageCore::ImageLoader::Instance().SetPriority(handle, priority);
    }

    void CancelDecode(ImageCore::ImageHandle handle)
    {
        if (IImageDecodeService* service = g_decodeServiceOverride.load())
        {
            service->Cancel(handle);
            return;
        }
        ImageCore::ImageLoader::Instance().Cancel(handle);
    }

    void RequestRedrawFromState(const std::shared_ptr<ImageAsyncBindingState>& state)
    {
        if (!state)
        {
            return;
        }

        std::shared_ptr<FD2D::AsyncRedrawToken> token;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            token = state->redrawToken;
        }
        if (token)
        {
            token->RequestAsyncRedraw();
        }
    }
}

ImageAsyncBindingRegistry& ImageAsyncBindingRegistry::Instance()
{
    static ImageAsyncBindingRegistry instance;
    return instance;
}

ImageAsyncBindingRegistry::Handle ImageAsyncBindingRegistry::Register(const std::shared_ptr<ImageAsyncBindingState>& state)
{
    if (!state)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const Handle handle = ++m_nextHandle;
    m_entries.emplace(handle, state);
    return handle;
}

void ImageAsyncBindingRegistry::Unregister(Handle handle)
{
    if (handle == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(handle);
}

void ImageAsyncBindingRegistry::InvalidateAll()
{
    std::vector<std::shared_ptr<ImageAsyncBindingState>> live;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        live.reserve(m_entries.size());
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            if (auto locked = it->second.lock())
            {
                live.push_back(std::move(locked));
                ++it;
            }
            else
            {
                // Drop expired entries while we hold the lock.
                it = m_entries.erase(it);
            }
        }
    }

    for (auto& state : live)
    {
        ImageAsyncBinding::InvalidateState(state);
    }
}

size_t ImageAsyncBindingRegistry::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

bool ImageAsyncBindingRegistry::IsRegistered(Handle handle) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.find(handle) != m_entries.end();
}

void ImageAsyncBindingRegistry::ShutdownPrepare()
{
    InvalidateAll();
}

ImageCore::ImageHandle ImageAsyncBinding::Supersede_NoLock(ImageAsyncBindingState& state)
{
    const ImageCore::ImageHandle handle = state.handle;
    state.handle = 0;
    if (state.phase == Phase::Loading || state.phase == Phase::Ready)
    {
        state.phase = Phase::Superseded;
    }
    state.pending = Payload {};
    state.demand.Reset();
    return handle;
}

void ImageAsyncBinding::InvalidateState(const std::shared_ptr<ImageAsyncBindingState>& state)
{
    if (!state)
    {
        return;
    }

    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->generation;
        handle = Supersede_NoLock(*state);
        state->requestPath.clear();
    }

    if (handle != 0)
    {
        CancelDecode(handle);
    }
}

ImageAsyncBinding::ImageAsyncBinding()
    : m_state(std::make_shared<ImageAsyncBindingState>())
{
    m_registration = ImageAsyncBindingRegistry::Instance().Register(m_state);
}

ImageAsyncBinding::~ImageAsyncBinding()
{
    ImageAsyncBindingRegistry::Instance().Unregister(m_registration);
    InvalidateState(m_state);
}

void ImageAsyncBinding::SetRedrawToken(std::shared_ptr<FD2D::AsyncRedrawToken> token)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->redrawToken = std::move(token);
}

void ImageAsyncBinding::RequestLoad(
    const ImageCore::ImageRequest& request,
    const std::wstring& normalizedPath)
{
    RequestLoad(request, normalizedPath, ImageCore::DefaultPriority(request.purpose));
}

void ImageAsyncBinding::SetPriority(ImageCore::RequestPriority priority)
{
    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->phase != Phase::Loading || m_state->priority == priority)
        {
            return;
        }
        m_state->priority = priority;
        handle = m_state->handle;
    }
    // A handle not stored yet (RequestDecode still returning) keeps the
    // dispatch priority; RequestLoad re-applies the current one once stored.
    if (handle != 0)
    {
        SetDecodePriority(handle, priority);
    }
}

void ImageAsyncBinding::RequestLoad(
    const ImageCore::ImageRequest& request,
    const std::wstring& normalizedPath,
    ImageCore::RequestPriority priority)
{
    // Registered before the dispatch: ImageCore may read at once.
    ArchiveEntryHandoff::Demand demand;
    if (request.purpose == ImageCore::ImagePurpose::Thumbnail || request.purpose == ImageCore::ImagePurpose::FullResolution)
    {
        demand = ArchiveEntryHandoff::Shared().Register(request.source,
            request.purpose == ImageCore::ImagePurpose::Thumbnail
                ? ArchiveEntryHandoff::DemandKind::Thumbnail
                : ArchiveEntryHandoff::DemandKind::MainImage);
    }

    uint64_t gen = 0;
    ImageCore::ImageHandle oldHandle = 0;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        oldHandle = Supersede_NoLock(*m_state);
        gen = ++m_state->generation;
        m_state->phase = Phase::Loading;
        m_state->requestPath = normalizedPath;
        m_state->priority = priority;
        m_state->demand = std::move(demand);
    }

    if (oldHandle != 0)
    {
        CancelDecode(oldHandle);
    }

    // The callback may run on a worker thread before RequestDecode returns,
    // after a newer request, or after Cancel; the generation decides.
    auto state = m_state;
    const ImageCore::ImageHandle handle = RequestDecode(
        request,
        [state, gen](HRESULT hr, ImageCore::DecodedImage image)
        {
            OnDecodeComplete(state, gen, hr, std::move(image));
        },
        priority);

    bool cancelNewHandle = false;
    bool rejected = false;
    bool reapplyPriority = false;
    ImageCore::RequestPriority currentPriority = priority;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (gen != m_state->generation)
        {
            // Superseded (or canceled) while dispatching.
            cancelNewHandle = (handle != 0);
        }
        else if (m_state->phase == Phase::Loading)
        {
            if (handle != 0)
            {
                m_state->handle = handle;
                // SetPriority while dispatching could not reach the handle yet.
                currentPriority = m_state->priority;
                reapplyPriority = (currentPriority != priority);
            }
            else
            {
                // ImageCore rejected the request and will never call back.
                m_state->phase = Phase::Failed;
                m_state->demand.Reset();
                m_state->failedPath = normalizedPath;
                m_state->failedHr = E_ABORT;
                rejected = true;
            }
        }
        // Otherwise the request already completed (Ready/Failed) before
        // RequestDecode returned; its handle is finished and is not stored.
    }

    if (cancelNewHandle)
    {
        CancelDecode(handle);
    }
    if (reapplyPriority)
    {
        SetDecodePriority(handle, currentPriority);
    }
    if (rejected)
    {
        FIC2_LOG_WARN("[ImageAsyncBinding] Decode request rejected: {}", AppLog::ToUtf8(normalizedPath));
        RequestRedrawFromState(m_state);
    }
}

void ImageAsyncBinding::OnDecodeComplete(
    const std::shared_ptr<ImageAsyncBindingState>& state,
    uint64_t gen,
    HRESULT hr,
    ImageCore::DecodedImage image)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // Only the current generation's in-flight request may change state; a
        // stale success or failure (superseded, canceled, or arriving after
        // Cancel returned) is dropped here as one atomic decision.
        if (gen != state->generation || state->phase != Phase::Loading)
        {
            return;
        }

        state->handle = 0;
        state->demand.Reset();
        const std::wstring normalizedSource = CommonUtil::NormalizePath(state->requestPath);

        if (SUCCEEDED(hr) && image.blocks && !image.blocks->empty())
        {
            state->pending.blocks = std::move(image.blocks);
            state->pending.width = image.width;
            state->pending.height = image.height;
            state->pending.rowPitch = image.rowPitchBytes;
            state->pending.format = image.dxgiFormat;
            state->pending.alphaEncoding = image.alphaEncoding;
            state->pending.alphaUsageHint = image.alphaUsageHint;
            state->pending.sourceWasBlockCompressed = image.sourceWasBlockCompressed;
            state->pending.sourceMipLevels = (std::max)(1u, image.sourceMipLevels);
            state->pending.sourceMipIndex = image.sourceMipIndex;
            state->pending.sourcePath = normalizedSource;
            state->failedPath.clear();
            state->failedHr = S_OK;
            state->phase = Phase::Ready;
        }
        else
        {
            state->failedPath = normalizedSource;
            state->failedHr = FAILED(hr) ? hr : E_FAIL;
            state->phase = Phase::Failed;
        }
    }

    RequestRedrawFromState(state);
}

void ImageAsyncBinding::Cancel()
{
    InvalidateState(m_state);
}

void ImageAsyncBinding::ClearPending()
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->phase == Phase::Ready)
    {
        m_state->pending = Payload {};
        m_state->phase = Phase::Superseded;
    }
}

bool ImageAsyncBinding::IsLoading() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->phase == Phase::Loading || m_state->phase == Phase::Ready;
}

void ImageAsyncBinding::MarkSettled()
{
    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->phase != Phase::Loading && m_state->phase != Phase::Ready)
        {
            return;
        }
        // A completion of the abandoned request must not apply later.
        ++m_state->generation;
        handle = Supersede_NoLock(*m_state);
    }

    if (handle != 0)
    {
        CancelDecode(handle);
    }
}

ImageAsyncBinding::Phase ImageAsyncBinding::CurrentPhase() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->phase;
}

bool ImageAsyncBinding::IsFailedFor(const std::wstring& path) const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return !m_state->failedPath.empty() && m_state->failedPath == path;
}

bool ImageAsyncBinding::ClearFailureIfMatches(const std::wstring& path, bool requireFailedHr)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    const bool matches = !m_state->failedPath.empty() && m_state->failedPath == path;
    if (!matches || (requireFailedHr && !FAILED(m_state->failedHr)))
    {
        return false;
    }

    m_state->failedPath.clear();
    m_state->failedHr = S_OK;
    return true;
}

void ImageAsyncBinding::ClearFailure()
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->failedPath.clear();
    m_state->failedHr = S_OK;
}

void ImageAsyncBinding::RecordFailure(const std::wstring& path, HRESULT hr)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->failedPath = path;
    m_state->failedHr = hr;
}

ImageAsyncBinding::Payload ImageAsyncBinding::TakePending()
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->phase != Phase::Ready)
    {
        return Payload {};
    }
    Payload out = std::move(m_state->pending);
    m_state->pending = Payload {};
    m_state->phase = Phase::Idle;
    return out;
}

bool ImageAsyncBinding::PollAndApply(const ApplyCallback& apply)
{
    if (!apply)
    {
        return false;
    }

    Payload pending = TakePending();
    if (!pending.blocks || pending.blocks->empty())
    {
        return false;
    }

    apply(std::move(pending));
    return true;
}

bool ImageAsyncBinding::HasPendingCpuBgra8For(const std::wstring& path) const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->phase == Phase::Ready
        && m_state->pending.blocks
        && IsCpuBgra8Format(m_state->pending.format)
        && m_state->pending.sourcePath == path;
}

void ImageAsyncBinding::SetDecodeServiceForTesting(IImageDecodeService* service)
{
    g_decodeServiceOverride.store(service);
}

HRESULT ImageAsyncBinding::CreateD2DBitmap(
    ID2D1RenderTarget* target,
    const Payload& payload,
    D2D1_ALPHA_MODE alphaMode,
    Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap)
{
    if (target == nullptr || !payload.blocks ||
        !IsCpuBgra8Format(payload.format) ||
        payload.width == 0 || payload.height == 0 || payload.rowPitch == 0)
    {
        return E_FAIL;
    }

    D2D1_BITMAP_PROPERTIES props {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = alphaMode;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    const D2D1_SIZE_U size = D2D1::SizeU(payload.width, payload.height);
    FIC2_TIMER_START(t_bmp);
    const HRESULT hr = target->CreateBitmap(
        size,
        payload.blocks->data(),
        payload.rowPitch,
        &props,
        &bitmap);
    const auto bmpMs = FIC2_ELAPSED_MS(t_bmp);
    if (bmpMs > 30)
    {
        FIC2_LOG_INFO("[D2D] CreateBitmap {}x{} took {}ms", payload.width, payload.height, bmpMs);
    }

    if (FAILED(hr) || !bitmap)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    outBitmap = bitmap;
    return S_OK;
}
