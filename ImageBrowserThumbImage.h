#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageCore/ImageRequest.h"
#include "ImageSourceIdentity.h"
#include "SelectionStyle.h"

#include <functional>
#include <memory>
#include <string>
#include <wrl/client.h>

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
    // The cache of decoded payloads: a presented payload is stored there (and
    // pinned while this control shows it), and a newly bound file with a
    // cached payload is shown from it instead of being requested again. Set
    // before SetSourceFile. Without one, the control holds its payload alone.
    void SetPayloadCache(std::shared_ptr<ThumbPayloadCache> cache) { m_payloadCache = std::move(cache); }
    // Binds `filePath`, listed with `identity` (ThumbModelEntry::identity).
    // Cache lookups use that identity and never read the file; an unknown
    // identity is neither looked up nor cached.
    HRESULT SetSourceFile(const std::wstring& filePath, const ImageSourceIdentity& identity = {});
    // The bound file was listed again with `identity`. When that differs from
    // the identity it was bound with (the file was replaced), the control
    // rebinds: the current thumbnail stays until the new one is shown.
    void SetSourceIdentity(const ImageSourceIdentity& identity);
    // Prepares the control for another file (tile recycling): cancels the
    // request in flight (the binding's generation drops its late completion),
    // and releases the bitmap, its pin on the CPU payload and the selection. A
    // payload decoded but not yet presented is stored in the payload cache
    // first (unpinned); cached payloads are kept, subject to the cache bounds.
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
    bool AdoptCachedPayload();
    // Pins `payload` (decoded for m_requestedBox) in the cache, or holds it
    // alone without one.
    ThumbPayloadCache::Pin StorePayload(const ImageAsyncBinding::Payload& payload);
    static bool SameListing(const ImageSourceIdentity& a, const ImageSourceIdentity& b);
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
    ImageSourceIdentity m_identity {};   // of m_filePath, as listed
    std::wstring m_loadedFilePath {};
    // The box of the request in flight (recorded with its cache entry).
    ImageCore::Size m_requestedBox { 0.0f, 0.0f };

    // The CPU payload shown (or about to be uploaded): the tile's only copy,
    // kept for device-loss and target recovery, pinned in the cache.
    ThumbPayloadCache::Pin m_payload {};
    bool m_needsCpuReupload { false };
    std::shared_ptr<ThumbPayloadCache> m_payloadCache {};

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
