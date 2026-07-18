#pragma once

// ImageAsyncBinding: FICture2-owned lifetime-safe async image load binding.
//
// Fixes the FD2D::AsyncImagePipeline lifetime hazard by:
//   - Keeping all mutable worker-visible state in a shared_ptr<State>
//   - Capturing only shared_ptr<State> (+ path/gen) in decode callbacks
//   - Using FD2D::AsyncRedrawToken instead of a raw Backplate*
//
// Threading:
//   - RequestLoad / Cancel / TakePending / PollAndApply run on the UI thread
//   - Decode completion runs on a worker thread and only stages payload / failure
//   - Stale completions are dropped via generation + path gating

#include "ImageCore/ImageRequest.h"
#include "ImageCore/ImageLoader.h"
#include "ImageCore/DecodedImage.h"
#include "FD2D/Backplate.h"

#include <d2d1.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ImageAsyncBinding;

// Global registry so app shutdown can invalidate all in-flight bindings
// before ImageCore::ImageLoader::Shutdown().
class ImageAsyncBindingRegistry
{
public:
    static ImageAsyncBindingRegistry& Instance();

    ImageAsyncBindingRegistry(const ImageAsyncBindingRegistry&) = delete;
    ImageAsyncBindingRegistry& operator=(const ImageAsyncBindingRegistry&) = delete;

    void Register(const std::shared_ptr<struct ImageAsyncBindingState>& state);
    void Unregister(const struct ImageAsyncBindingState* state);

    // Bump generations / cancel handles / clear pending / stop loading.
    void InvalidateAll();

    // Alias for InvalidateAll — call before ImageLoader::Shutdown.
    void ShutdownPrepare();

private:
    ImageAsyncBindingRegistry() = default;

    mutable std::mutex m_mutex;
    std::vector<std::weak_ptr<struct ImageAsyncBindingState>> m_entries;
};

struct ImageAsyncBindingState
{
    std::atomic<uint64_t> generation { 0 };
    std::atomic<uint64_t> inflightGeneration { 0 };
    std::atomic<bool> loading { false };

    mutable std::mutex mutex;
    ImageCore::ImageHandle handle { 0 };
    std::wstring inflightPath {};
    std::shared_ptr<FD2D::AsyncRedrawToken> redrawToken {};

    struct Payload
    {
        std::shared_ptr<std::vector<uint8_t>> blocks {};
        uint32_t width { 0 };
        uint32_t height { 0 };
        uint32_t rowPitch { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        ImageCore::AlphaEncoding alphaEncoding { ImageCore::AlphaEncoding::Unknown };
        ImageCore::AlphaUsage alphaUsageHint { ImageCore::AlphaUsage::Auto };
        bool sourceWasBlockCompressed { false };
        std::wstring sourcePath {};
    };

    Payload pending {};
    std::wstring failedPath {};
    HRESULT failedHr { S_OK };
};

class ImageAsyncBinding
{
public:
    using Payload = ImageAsyncBindingState::Payload;
    using ApplyCallback = std::function<void(Payload&&)>;

    ImageAsyncBinding();
    ~ImageAsyncBinding();

    ImageAsyncBinding(const ImageAsyncBinding&) = delete;
    ImageAsyncBinding& operator=(const ImageAsyncBinding&) = delete;

    void SetRedrawToken(std::shared_ptr<FD2D::AsyncRedrawToken> token);

    // Cancels any prior in-flight load, then dispatches a new decode.
    void RequestLoad(const ImageCore::ImageRequest& request, const std::wstring& normalizedPath);

    // Bump generation, cancel handle, clear pending, set loading false.
    void Cancel();

    void ClearPending();

    bool IsLoading() const;
    void SetLoading(bool loading);

    bool IsFailedFor(const std::wstring& path) const;
    bool ClearFailureIfMatches(const std::wstring& path, bool requireFailedHr = false);
    void ClearFailure();
    void RecordFailure(const std::wstring& path, HRESULT hr);

    // UI thread only.
    Payload TakePending();

    // UI thread only. Returns true when a non-empty payload was applied.
    bool PollAndApply(const ApplyCallback& apply);

    bool HasPendingCpuBgra8For(const std::wstring& path) const;

    static HRESULT CreateD2DBitmap(
        ID2D1RenderTarget* target,
        const Payload& payload,
        D2D1_ALPHA_MODE alphaMode,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

    static bool IsCpuBgra8Format(DXGI_FORMAT fmt)
    {
        return fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

private:
    friend class ImageAsyncBindingRegistry;

    static void InvalidateState(const std::shared_ptr<ImageAsyncBindingState>& state);
    static void OnDecodeComplete(
        const std::shared_ptr<ImageAsyncBindingState>& state,
        uint64_t gen,
        const std::wstring& requestedPath,
        HRESULT hr,
        ImageCore::DecodedImage image);

    std::shared_ptr<ImageAsyncBindingState> m_state {};
};
