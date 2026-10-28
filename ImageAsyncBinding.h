#pragma once

// ImageAsyncBinding: FICture2-owned lifetime-safe async image load binding.
//
// Fixes the FD2D::AsyncImagePipeline lifetime hazard by:
//   - Keeping all mutable worker-visible state in a shared_ptr<State>
//   - Capturing only shared_ptr<State> (+ path/gen) in decode callbacks
//   - Using FD2D::AsyncRedrawToken instead of a raw Backplate*
//
// Threading:
//   - RequestLoad / Cancel / TakePending / PollAndApply / MarkSettled run on the UI thread
//   - Decode completion runs on a worker thread and only stages payload / failure
//   - Every transition (generation, phase, path, handle, payload, failure) happens
//     under ImageAsyncBindingState::mutex as one logical change; a completion is
//     applied only while its generation is current and the phase is Loading.
//
// States (ImageAsyncBindingState::Phase):
//   Idle       - no request outstanding (initial, or the staged payload was taken)
//   Loading    - the current generation's decode is in flight
//   Ready      - the current generation's payload is staged for TakePending
//   Failed     - the current generation's decode failed (or was rejected)
//   Superseded - the request was canceled or abandoned before its result was used
// Presentation states such as "displayed" belong to the owning control.
//
// ImageCore contract: a decode callback can arrive after Cancel returns (and
// concurrently with a newer request). Such callbacks carry an old generation
// and are dropped without touching the newer request's state.

#include "ArchiveEntryHandoff.h"
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
#include <unordered_map>
#include <vector>

class ImageAsyncBinding;

// Decode backend used by ImageAsyncBinding. Production uses CloudThumbnailRouter
// (shell thumbnails of online-only cloud files) in front of ImageCore::ImageLoader;
// tests install a fake through ImageAsyncBinding::SetDecodeServiceForTesting to
// control when and in which order completions are delivered.
class IImageDecodeService
{
public:
    virtual ~IImageDecodeService() = default;

    virtual ImageCore::ImageHandle RequestDecoded(
        const ImageCore::ImageRequest& request,
        ImageCore::DecodedImageLoadCallback callback,
        ImageCore::RequestPriority priority) = 0;
    virtual void Cancel(ImageCore::ImageHandle handle) = 0;
    // Promotes or demotes a queued request (ImageCore::ImageLoader::SetPriority).
    virtual void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) = 0;
};

// Global registry so app shutdown can invalidate all in-flight bindings
// before ImageCore::ImageLoader::Shutdown().
//
// Registration is handle-based: Register returns a handle and Unregister
// removes that entry directly, so neither is linear in the number of live
// bindings (thumbnail tiles register and unregister continuously while the
// strip scrolls).
class ImageAsyncBindingRegistry
{
public:
    using Handle = uint64_t;   // 0: not registered

    static ImageAsyncBindingRegistry& Instance();

    // Bindings use Instance(); tests may create their own registry.
    ImageAsyncBindingRegistry() = default;
    ImageAsyncBindingRegistry(const ImageAsyncBindingRegistry&) = delete;
    ImageAsyncBindingRegistry& operator=(const ImageAsyncBindingRegistry&) = delete;

    // Returns the entry's handle (0 for a null state).
    Handle Register(const std::shared_ptr<struct ImageAsyncBindingState>& state);
    // Removes the entry of `handle`; unknown or repeated handles are ignored.
    void Unregister(Handle handle);

    // Supersede every live binding (Cancel on each: generation bump, handle cancel, payload drop).
    void InvalidateAll();

    // Alias for InvalidateAll — call before ImageLoader::Shutdown.
    void ShutdownPrepare();

    // Registered entries (diagnostics and tests).
    size_t Count() const;
    bool IsRegistered(Handle handle) const;

private:
    mutable std::mutex m_mutex;
    Handle m_nextHandle { 0 };
    std::unordered_map<Handle, std::weak_ptr<struct ImageAsyncBindingState>> m_entries;
};

struct ImageAsyncBindingState
{
    enum class Phase
    {
        Idle,
        Loading,
        Ready,
        Failed,
        Superseded,
    };

    // All fields are guarded by `mutex`.
    mutable std::mutex mutex;
    Phase phase { Phase::Idle };
    uint64_t generation { 0 };          // bumped by every request and invalidation
    ImageCore::ImageHandle handle { 0 }; // in-flight handle of `generation`, 0 when none
    ImageCore::RequestPriority priority { ImageCore::RequestPriority::Visible };  // of `generation`
    std::wstring requestPath {};         // normalized path of `generation`'s request
    // `generation`'s demand on its archive entry (ArchiveEntryHandoff::Shared()),
    // held while Loading: a solid-archive thumbnail pass parks the bytes of
    // demanded entries only, and a main image is read at High priority.
    ArchiveEntryHandoff::Demand demand {};
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
        uint32_t sourceMipLevels { 1 };
        uint32_t sourceMipIndex { 0 };
        std::wstring sourcePath {};
    };

    Payload pending {};                  // staged while phase == Ready
    // Failure memo used by the owning control to avoid retry loops; set by a
    // failed decode or RecordFailure, cleared by a successful decode or the
    // Clear* calls. It outlives the phase that produced it.
    std::wstring failedPath {};
    HRESULT failedHr { S_OK };
};

class ImageAsyncBinding
{
public:
    using Payload = ImageAsyncBindingState::Payload;
    using Phase = ImageAsyncBindingState::Phase;
    using ApplyCallback = std::function<void(Payload&&)>;

    ImageAsyncBinding();
    ~ImageAsyncBinding();

    ImageAsyncBinding(const ImageAsyncBinding&) = delete;
    ImageAsyncBinding& operator=(const ImageAsyncBinding&) = delete;

    void SetRedrawToken(std::shared_ptr<FD2D::AsyncRedrawToken> token);

    // Supersedes any prior request (canceling its handle), then dispatches a new
    // decode at `priority` (ImageCore::DefaultPriority of the purpose when
    // omitted): phase Loading. A request ImageCore rejects (handle 0, for
    // example after shutdown) ends in Failed with E_ABORT recorded for the path.
    void RequestLoad(const ImageCore::ImageRequest& request, const std::wstring& normalizedPath);
    void RequestLoad(
        const ImageCore::ImageRequest& request,
        const std::wstring& normalizedPath,
        ImageCore::RequestPriority priority);

    // Changes the priority of the in-flight request (no-op unless Loading; the
    // next RequestLoad takes its own priority). ImageCore applies it while the
    // request still waits in a queue.
    void SetPriority(ImageCore::RequestPriority priority);

    // Invalidates the current request: Loading/Ready -> Superseded (in-flight
    // handle canceled, staged payload dropped). The failure memo is kept.
    void Cancel();

    // Drops a staged payload: Ready -> Superseded.
    void ClearPending();

    // True while a request is in flight or its payload is staged but not yet
    // taken (Loading or Ready).
    bool IsLoading() const;

    // The owning control is done with the current request (it displayed the
    // result, showed cached content, or will issue a new request). A request
    // still in flight or staged is superseded; Failed and Idle are unchanged.
    void MarkSettled();

    Phase CurrentPhase() const;

    bool IsFailedFor(const std::wstring& path) const;
    bool ClearFailureIfMatches(const std::wstring& path, bool requireFailedHr = false);
    void ClearFailure();
    void RecordFailure(const std::wstring& path, HRESULT hr);

    // UI thread only. Takes the staged payload (Ready -> Idle); empty otherwise.
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

    // Tests only: route decode requests/cancels to `service` (nullptr restores
    // ImageCore::ImageLoader). Install before creating bindings that use it.
    static void SetDecodeServiceForTesting(IImageDecodeService* service);

private:
    friend class ImageAsyncBindingRegistry;

    // Moves a Loading or Ready request to Superseded (dropping any staged
    // payload) and returns the in-flight handle to cancel, 0 if none. Requires
    // state.mutex; the caller cancels the handle after unlocking.
    static ImageCore::ImageHandle Supersede_NoLock(ImageAsyncBindingState& state);

    static void InvalidateState(const std::shared_ptr<ImageAsyncBindingState>& state);
    static void OnDecodeComplete(
        const std::shared_ptr<ImageAsyncBindingState>& state,
        uint64_t gen,
        HRESULT hr,
        ImageCore::DecodedImage image);

    std::shared_ptr<ImageAsyncBindingState> m_state {};
    ImageAsyncBindingRegistry::Handle m_registration { 0 };
};
