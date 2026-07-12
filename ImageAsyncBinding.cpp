#include "ImageAsyncBinding.h"
#include "AppLog.h"
#include "CommonUtil.h"
#include "ImageCore/DecodedImage.h"

#include <algorithm>
#include <chrono>

namespace
{
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

void ImageAsyncBindingRegistry::Register(const std::shared_ptr<ImageAsyncBindingState>& state)
{
    if (!state)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.push_back(state);
}

void ImageAsyncBindingRegistry::Unregister(const ImageAsyncBindingState* state)
{
    if (!state)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(
        std::remove_if(
            m_entries.begin(),
            m_entries.end(),
            [state](const std::weak_ptr<ImageAsyncBindingState>& weak)
            {
                auto locked = weak.lock();
                return !locked || locked.get() == state;
            }),
        m_entries.end());
}

void ImageAsyncBindingRegistry::InvalidateAll()
{
    std::vector<std::shared_ptr<ImageAsyncBindingState>> live;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        live.reserve(m_entries.size());
        for (auto& weak : m_entries)
        {
            if (auto locked = weak.lock())
            {
                live.push_back(std::move(locked));
            }
        }
        // Drop expired weak_ptrs while we hold the lock.
        m_entries.erase(
            std::remove_if(
                m_entries.begin(),
                m_entries.end(),
                [](const std::weak_ptr<ImageAsyncBindingState>& weak)
                {
                    return weak.expired();
                }),
            m_entries.end());
    }

    for (auto& state : live)
    {
        ImageAsyncBinding::InvalidateState(state);
    }
}

void ImageAsyncBindingRegistry::ShutdownPrepare()
{
    InvalidateAll();
}

void ImageAsyncBinding::InvalidateState(const std::shared_ptr<ImageAsyncBindingState>& state)
{
    if (!state)
    {
        return;
    }

    state->generation.fetch_add(1ULL);
    state->inflightGeneration.store(0);

    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        handle = state->handle;
        state->handle = 0;
        state->inflightPath.clear();
        state->pending = Payload {};
    }

    if (handle != 0)
    {
        ImageCore::ImageLoader::Instance().Cancel(handle);
    }

    state->loading.store(false);
}

ImageAsyncBinding::ImageAsyncBinding()
    : m_state(std::make_shared<ImageAsyncBindingState>())
{
    ImageAsyncBindingRegistry::Instance().Register(m_state);
}

ImageAsyncBinding::~ImageAsyncBinding()
{
    ImageAsyncBindingRegistry::Instance().Unregister(m_state.get());
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
    const uint64_t gen = m_state->generation.fetch_add(1ULL) + 1ULL;

    ImageCore::ImageHandle oldHandle = 0;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        oldHandle = m_state->handle;
        m_state->handle = 0;
        m_state->pending = Payload {};
        m_state->inflightPath = normalizedPath;
    }

    if (oldHandle != 0)
    {
        ImageCore::ImageLoader::Instance().Cancel(oldHandle);
    }

    m_state->inflightGeneration.store(gen);
    m_state->loading.store(true);

    auto state = m_state;
    const ImageCore::ImageHandle handle = ImageCore::ImageLoader::Instance().RequestDecoded(
        request,
        [state, gen, normalizedPath](HRESULT hr, ImageCore::DecodedImage image)
        {
            // Worker thread — capture only shared_ptr<State> (+ path, gen).
            if (gen != state->inflightGeneration.load())
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->inflightPath != normalizedPath)
                {
                    return;
                }
            }

            if (gen != state->inflightGeneration.load())
            {
                return;
            }

            OnDecodeComplete(state, gen, normalizedPath, hr, std::move(image));
        });

    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (gen == m_state->inflightGeneration.load() && m_state->inflightPath == normalizedPath)
        {
            m_state->handle = handle;
        }
        else if (handle != 0)
        {
            // Cancelled / superseded before we could store the handle.
            ImageCore::ImageLoader::Instance().Cancel(handle);
        }
    }
}

void ImageAsyncBinding::OnDecodeComplete(
    const std::shared_ptr<ImageAsyncBindingState>& state,
    uint64_t gen,
    const std::wstring& requestedPath,
    HRESULT hr,
    ImageCore::DecodedImage image)
{
    if (gen != state->inflightGeneration.load())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->inflightPath != requestedPath)
        {
            return;
        }
        state->handle = 0;
    }

    const std::wstring normalizedSource = CommonUtil::NormalizePath(requestedPath);

    if (SUCCEEDED(hr) && image.blocks && !image.blocks->empty())
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // Re-check under lock after staging decision.
        if (gen != state->inflightGeneration.load() || state->inflightPath != requestedPath)
        {
            return;
        }
        state->pending.blocks = std::move(image.blocks);
        state->pending.width = image.width;
        state->pending.height = image.height;
        state->pending.rowPitch = image.rowPitchBytes;
        state->pending.format = image.dxgiFormat;
        state->pending.sourcePath = normalizedSource;
        state->failedPath.clear();
        state->failedHr = S_OK;
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (gen != state->inflightGeneration.load() || state->inflightPath != requestedPath)
            {
                return;
            }
            state->failedPath = normalizedSource;
            state->failedHr = hr;
        }
        state->loading.store(false);
        state->inflightGeneration.store(0);
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
    m_state->pending = Payload {};
}

bool ImageAsyncBinding::IsLoading() const
{
    return m_state->loading.load();
}

void ImageAsyncBinding::SetLoading(bool loading)
{
    m_state->loading.store(loading);
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
    Payload out = std::move(m_state->pending);
    m_state->pending = Payload {};
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
    m_state->loading.store(false);
    m_state->inflightGeneration.store(0);
    return true;
}

bool ImageAsyncBinding::HasPendingCpuBgra8For(const std::wstring& path) const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->pending.blocks
        && IsCpuBgra8Format(m_state->pending.format)
        && m_state->pending.sourcePath == path;
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
