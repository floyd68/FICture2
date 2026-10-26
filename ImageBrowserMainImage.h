#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAlphaPresentation.h"
#include "ImageAsyncBinding.h"
#include "ImagePresentationRecovery.h"
#include "ImageSourceIdentity.h"
#include "ImageViewTypes.h"
#include "ImageCore/ImageRequest.h"

#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Creates an immutable single-mip texture and its SRV on `device` from a
// block-compressed (BCn) payload: the main view's GPU passthrough. E_INVALIDARG
// for a null device or a payload that is not block-compressed.
HRESULT CreateCompressedShaderResource(
    ID3D11Device* device,
    const ImageAsyncBinding::Payload& payload,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv);

// FICture2 main-image control: owns FD2D::Image + Spinner + ImageAsyncBinding.
// Ports FullResolution behavior from FD2D::Image (zoom/pan/rotation/GPU BCn).
class ImageBrowserMainImage : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;
    using ViewChangedHandler = std::function<void(const ImageViewTransform&)>;
    // Called on the UI thread whenever the presented image path changes: the
    // path of a newly presented image, or "" when the content is cleared/lost.
    using PresentedPathHandler = std::function<void(const std::wstring&)>;
    // Called on the UI thread when anything shown in the image information
    // (loaded metadata, mip, alpha, channel, sampling, zoom %, rotation) changes.
    using InfoChangedHandler = std::function<void()>;

    ImageBrowserMainImage();
    explicit ImageBrowserMainImage(const std::wstring& name);
    ~ImageBrowserMainImage() override;

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;

    // Same path: S_FALSE while it is already shown or loading - unless the file
    // (or its host archive) changed since it was decoded, which reloads it.
    HRESULT SetSourceFile(const std::wstring& filePath);
    // Drops cached GPU content for the current file and decodes it again from
    // mip 0 (the only decode that supplies the full-source size and mip count).
    void ReloadSource();
    void ClearSource();

    void SetOnClick(ClickHandler handler);

    void SetLoadingSpinnerEnabled(bool enabled);
    bool LoadingSpinnerEnabled() const { return m_loadingSpinnerEnabled; }

    void SetZoomScale(float scale);
    float ZoomScale() const { return m_zoomScale; }
    void ResetZoom();
    void SetZoomStiffness(float stiffness);
    float ZoomStiffness() const { return m_zoomStiffness; }
    void AdvanceZoomAnimation(unsigned long long nowMs);

    ImageLoadedInfo GetLoadedInfo() const;

    void SetInteractionEnabled(bool enabled);
    bool InteractionEnabled() const { return m_interactionEnabled; }

    ImageViewTransform GetViewTransform() const;
    void SetViewTransform(const ImageViewTransform& vt, bool notify = true);
    void SetOnViewChanged(ViewChangedHandler handler);
    void SetOnPresentedPathChanged(PresentedPathHandler handler);
    void SetOnInfoChanged(InfoChangedHandler handler);

    void RotateCW();
    void RotateCCW();

    bool SelectMip(uint32_t mipLevel);
    uint32_t MipLevel() const;
    uint32_t MipLevels() const;

    // 0=RGBA, 1=R, 2=G, 3=B, 4=A. Re-selecting the active mode returns to RGBA.
    void SetChannelMode(int mode);
    int ChannelMode() const { return m_channelMode; }

    void SetAlphaUsageOverride(ImageCore::AlphaUsage usage);
    ImageCore::AlphaUsage AlphaUsageOverride() const { return m_alphaUsageOverride; }
    ImageCore::AlphaUsage EffectiveAlphaUsage() const;

    void SetAlphaCheckerboardEnabled(bool enabled);
    bool AlphaCheckerboardEnabled() const { return m_alphaCheckerboardEnabled; }

    void SetHighQualitySampling(bool enabled);
    bool HighQualitySampling() const { return m_highQualitySampling; }
    void ToggleSamplingQuality();

    bool GpuPresentation() const { return m_gpuPresentation; }
    std::wstring InformationText() const;

    void OnAttached(FD2D::Backplate& backplate) override;
    void OnGraphicsInvalidated(
        FD2D::GraphicsInvalidationReason reason,
        const FD2D::GraphicsGeneration& generation) override;
    void OnRender(ID2D1RenderTarget* target) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    // A GPU-cache entry about to be presented (cache hits of SetSourceFile and
    // SelectMip).
    struct CachedPresentation
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
        std::wstring path {};
        uint32_t width { 0 };
        uint32_t height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        ImageAlphaInfo alpha {};
        uint32_t sourceMipLevels { 1 };
        uint32_t sourceMipIndex { 0 };
        ImageSourceIdentity identity {};
        uint64_t deviceGeneration { 0 };
        // A new source (SetSourceFile) rather than another mip of the shown one.
        bool replacesSource { false };
    };

    void InitChildren();
    void RequestImageLoad();
    void SyncTextureDrawState();
    // The frame's presentation handoff (see OnRenderD3D): installs a staged
    // cache hit, a completed decode or a CPU re-upload before anything of this
    // control is drawn in the frame, so both passes draw the same content.
    void PresentPendingContent(ID2D1RenderTarget* target);
    void ApplyPendingPayload(ID2D1RenderTarget* target);
    void TryReuploadCpuPayload(ID2D1RenderTarget* target);
    bool TryGetCachedPresentation(
        const std::wstring& path,
        uint32_t mipLevel,
        const ImageSourceIdentity& identity,
        CachedPresentation& out) const;
    // Presents a cache hit now, or at the next frame's handoff when this
    // frame's D3D pass has already drawn and its D2D pass has not.
    void PresentCached(CachedPresentation&& cached);
    void InstallCached(const CachedPresentation& cached);
    // Between this control's D3D pass and its D2D pass of the same frame.
    bool IsBetweenRenderPasses() const;
    bool HasStagedContentForCurrentRequest() const;
    bool TryGetContentSize(D2D1_SIZE_F& outSize) const;
    void ClampPanToVisible();
    bool HasDisplayedContentForCurrentPath() const;
    uint64_t DeviceGeneration() const;

    // The one place that records what is on screen (cache hits, GPU formats,
    // BGRA SRVs, D2D bitmaps, CPU re-upload): loaded metadata, presentation
    // kind, the resource-domain stamp it belongs to, and its source identity.
    void ApplyPresentation(
        PresentationKind kind,
        const std::wstring& path,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        const ImageAlphaInfo& alpha,
        uint32_t sourceMipLevels,
        uint32_t sourceMipIndex,
        const ImageSourceIdentity& identity);
    // Forgets the loaded metadata (the presented path is managed separately).
    void ResetPresentation();
    // Creates a B8G8R8A8 SRV from a CPU payload on the current D3D device.
    HRESULT CreateBgraShaderResource(
        const ImageAsyncBinding::Payload& payload,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) const;

    std::shared_ptr<FD2D::Image> m_texture {};
    std::shared_ptr<FD2D::Spinner> m_spinner {};
    ImageAsyncBinding m_binding {};
    ImageCore::ImageRequest m_request {};

    std::wstring m_filePath {};
    std::wstring m_loadedFilePath {};
    std::atomic<bool> m_forceCpuDecode { false };

    // Last applied CPU BGRA8 payload (kept across target/device invalidation for re-upload).
    ImageAsyncBinding::Payload m_cpuPayload {};
    bool m_needsCpuReupload { false };

    // A cache hit that arrived between this frame's passes (presented at the
    // next handoff), and whether this frame's handoff ran in the D3D pass.
    std::optional<CachedPresentation> m_stagedCacheHit {};
    bool m_presentedInD3DPass { false };

    uint32_t m_loadedW { 0 };
    uint32_t m_loadedH { 0 };
    DXGI_FORMAT m_loadedFormat { DXGI_FORMAT_UNKNOWN };
    ImageAlphaInfo m_alpha {};
    ImageCore::AlphaUsage m_alphaUsageOverride { ImageCore::AlphaUsage::Auto };
    uint32_t m_sourceMipLevels { 1 };
    uint32_t m_sourceMipIndex { 0 };
    uint32_t m_sourceWidth { 0 };
    uint32_t m_sourceHeight { 0 };
    bool m_selectingMip { false };
    bool m_gpuPresentation { false };
    PresentationKind m_presentationKind { PresentationKind::None };
    FD2D::GraphicsResourceStamp m_presentationStamp {};
    ImageSourceIdentity m_presentationIdentity {};
    // Identity of the file when the current decode was requested.
    ImageSourceIdentity m_requestIdentity {};
    // Mip to select once a reload of a replaced source has presented mip 0
    // (0 = none).
    uint32_t m_mipAfterReload { 0 };
    uint64_t m_lastSeenDeviceGeneration { 0 };
    int m_channelMode { 0 };

    bool m_alphaCheckerboardEnabled { false };
    bool m_interactionEnabled { true };
    bool m_highQualitySampling { true };

    ClickHandler m_onClick {};
    bool m_loadingSpinnerEnabled { true };

    float m_zoomScale { 1.0f };
    float m_targetZoomScale { 1.0f };
    float m_zoomVelocity { 0.0f };
    unsigned long long m_lastZoomAnimMs { 0 };
    float m_zoomStiffness { 100.0f };

    float m_panX { 0.0f };
    float m_panY { 0.0f };
    bool m_panning { false };
    bool m_panArmed { false };
    float m_panStartX { 0.0f };
    float m_panStartY { 0.0f };
    float m_panStartOffsetX { 0.0f };
    float m_panStartOffsetY { 0.0f };

    bool m_pointerZoomActive { false };
    float m_pointerZoomStartZoom { 1.0f };
    float m_pointerZoomStartPanX { 0.0f };
    float m_pointerZoomStartPanY { 0.0f };
    float m_pointerZoomMouseX { 0.0f };
    float m_pointerZoomMouseY { 0.0f };

    ViewChangedHandler m_onViewChanged {};
    PresentedPathHandler m_onPresentedPathChanged {};
    InfoChangedHandler m_onInfoChanged {};
    std::wstring m_lastInfoKey {};
    // Fires m_onInfoChanged when the information-relevant state differs from the
    // last notification (cheap; called wherever that state may change).
    void NotifyInfoIfChanged();

    // Single place where the presented path changes; notifies the owner.
    void SetLoadedFilePath(const std::wstring& path);
    bool m_suppressViewNotify { false };
    int m_rotationQuarters { 0 };
};
