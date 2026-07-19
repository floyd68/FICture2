#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAlphaPresentation.h"
#include "ImageAsyncBinding.h"
#include "ImageViewTypes.h"
#include "ImageCore/ImageRequest.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

// FICture2 main-image control: owns FD2D::Image + Spinner + ImageAsyncBinding.
// Ports FullResolution behavior from FD2D::Image (zoom/pan/rotation/GPU BCn).
class ImageBrowserMainImage : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;
    using ViewChangedHandler = std::function<void(const ImageViewTransform&)>;

    ImageBrowserMainImage();
    explicit ImageBrowserMainImage(const std::wstring& name);
    ~ImageBrowserMainImage() override;

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;

    HRESULT SetSourceFile(const std::wstring& filePath);
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
    void InitChildren();
    void RequestImageLoad();
    void SyncTextureDrawState();
    void ApplyPendingPayload(ID2D1RenderTarget* target);
    void TryReuploadCpuPayload(ID2D1RenderTarget* target);
    bool TryGetContentSize(D2D1_SIZE_F& outSize) const;
    void ClampPanToVisible();
    bool HasDisplayedContentForCurrentPath() const;
    uint64_t DeviceGeneration() const;

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
    bool m_suppressViewNotify { false };
    int m_rotationQuarters { 0 };
};
