#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAsyncBinding.h"
#include "ImageCore/ImageRequest.h"
#include "SelectionStyle.h"

#include <functional>
#include <memory>
#include <string>
#include <wrl/client.h>

class ThumbPayloadStore;

// FICture2 thumbnail image control: owns FD2D::Image + Spinner + ImageAsyncBinding.
// Ports FD2D::ThumbImage behavior with path-gated pending apply.
class ImageBrowserThumbImage : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;

    ImageBrowserThumbImage();
    explicit ImageBrowserThumbImage(const std::wstring& name);
    ~ImageBrowserThumbImage() override;

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;

    void SetThumbnailSize(const FD2D::Size& size);
    // Where decoded payloads are retained per item: a presented payload is
    // retained there, and a newly bound file with a retained payload is shown
    // from it instead of being requested again. Set before SetSourceFile.
    void SetPayloadStore(std::shared_ptr<ThumbPayloadStore> store) { m_payloadStore = std::move(store); }
    HRESULT SetSourceFile(const std::wstring& filePath);
    // Prepares the control for another file (tile recycling): cancels the
    // request in flight (the binding's generation drops its late completion),
    // and releases the bitmap, the CPU payload reference and the selection. A
    // payload decoded but not yet presented is retained in the payload store
    // first; retained payloads are kept.
    void Recycle();

    void SetSelected(bool selected);
    bool Selected() const { return m_selected; }
    void SetSelectionStyle(const FD2D::SelectionStyle& style);

    void SetOnClick(ClickHandler handler);
    void SetLoadingSpinnerEnabled(bool enabled);
    // Decode priority of this thumbnail: Prefetch until the strip reports it
    // on screen (Visible). Applied to a request in flight and to later ones.
    void SetLoadPriority(ImageCore::RequestPriority priority);
    ImageCore::RequestPriority LoadPriority() const { return m_loadPriority; }

    D2D1_SIZE_F GetBitmapSize() const;

    void OnAttached(FD2D::Backplate& backplate) override;
    void OnGraphicsInvalidated(
        FD2D::GraphicsInvalidationReason reason,
        const FD2D::GraphicsGeneration& generation) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    void InitChildren();
    void RequestImageLoad();
    bool AdoptRetainedPayload();
    void SyncTextureDrawState();
    void ApplyPendingPayload(ID2D1RenderTarget* target);
    void TryReuploadCpuPayload(ID2D1RenderTarget* target);
    void DrawSelectionOverlay(ID2D1RenderTarget* target);

    std::shared_ptr<FD2D::Image> m_texture {};
    std::shared_ptr<FD2D::Spinner> m_spinner {};
    ImageCore::RequestPriority m_loadPriority { ImageCore::RequestPriority::Prefetch };
    ImageAsyncBinding m_binding {};
    ImageCore::ImageRequest m_request {};

    std::wstring m_filePath {};
    std::wstring m_loadedFilePath {};

    ImageAsyncBinding::Payload m_cpuPayload {};
    bool m_needsCpuReupload { false };
    std::shared_ptr<ThumbPayloadStore> m_payloadStore {};

    bool m_selected { false };
    FD2D::SelectionStyle m_selectionStyle {};
    unsigned long long m_selectionAnimStartMs { 0 };
    unsigned long long m_selectionAnimMs { 150 };
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionShadowBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionFillBrush {};
    // Target generations the selection brushes and the presented bitmap were
    // created under (thumbnails present D2D bitmaps only).
    FD2D::GraphicsResourceStamp m_brushStamp {};
    FD2D::GraphicsResourceStamp m_bitmapStamp {};

    ClickHandler m_onClick {};
    bool m_loadingSpinnerEnabled { true };
};
