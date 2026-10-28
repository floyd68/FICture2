#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserThumbSizePolicy.h"
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

    // Fixed decode box (no size policy): the request targets `size` as is.
    void SetThumbnailSize(const FD2D::Size& size);
    // Strip row mode: the thumbnail is shown `rowHeight` tall and as wide as
    // its aspect, and is decoded by ThumbSizePolicy (bucketed request box,
    // one upgrade when the row grows past what is held). The current
    // thumbnail stays until a larger one arrives.
    void SetRowHeight(float rowHeight);
    // While set, undersized thumbnails are not upgraded (the row height is
    // still moving: splitter drag). Clearing it evaluates them again.
    void SetSizeUpgradesDeferred(bool deferred);
    // Called (with the decoded height) when an admitted decode was limited by
    // its source rather than its box: no larger bucket can be served.
    using SourceLimitedHandler = std::function<void(uint32_t height)>;
    void SetOnSourceLimited(SourceLimitedHandler handler) { m_onSourceLimited = std::move(handler); }
    // The cache of decoded payloads: a presented payload is stored there (and
    // pinned while this control shows it), and a newly bound file with a
    // cached payload is shown from it instead of being requested again. Set
    // before SetSourceFile. Without one, the control holds its payload alone.
    void SetPayloadCache(std::shared_ptr<ThumbPayloadCache> cache) { m_payloadCache = std::move(cache); }
    // Binds `filePath`, listed with `identity` (ThumbModelEntry::identity).
    // Cache lookups use that identity and never read the file; an unknown
    // identity is neither looked up nor cached. `hint` is what the item model
    // already knows of the item's decoded size (aspect, source-limited
    // height): a known aspect shapes the first request, so a non-square image
    // is not decoded twice.
    HRESULT SetSourceFile(
        const std::wstring& filePath,
        const ImageSourceIdentity& identity = {},
        const ThumbSizePolicy::ItemHint& hint = {});
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
    // Takes a completed decode off the binding and pins it in the payload
    // cache, so its bytes are accounted (and older unpinned entries make
    // room) before, and independently of, its bitmap upload: a prefetch tile
    // outside the viewport is not rendered, yet its payload is alive. The
    // bitmap is created from the pinned payload when the control renders.
    // The browser runs this for every bound tile once per frame, before the
    // tiles render; OnRender runs it too. Returns true when one was admitted.
    bool AdmitCompletedPayload();

    // Selection, style and spinner setters invalidate only on a change.
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
    // Requests a larger decode when the payload shown is undersized for the
    // row height (ThumbSizePolicy::Evaluate).
    void RequestSizeUpgradeIfDue();
    // The shown payload as the size policy sees it (empty: none).
    ThumbSizePolicy::Decoded ShownDecoded() const;
    bool AdoptCachedPayload();
    // The cached size of this item to show for `bucket` (see
    // ThumbSizePolicy::ChooseCached); with `onlyIfSatisfies`, none unless it
    // satisfies the bucket.
    ThumbPayloadCache::Pin AcquireCachedFor(float bucket, bool onlyIfSatisfies);
    // Pins `payload` (decoded for m_requestedBox) in the cache, or holds it
    // alone without one.
    ThumbPayloadCache::Pin StorePayload(const ImageAsyncBinding::Payload& payload);
    static bool SameListing(const ImageSourceIdentity& a, const ImageSourceIdentity& b);
    void SyncTextureDrawState();
    // Creates the bitmap from the pinned payload (a newly admitted decode, a
    // cached payload, or recovery after a device or target change).
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

    // Size policy state of the bound item (strip row mode when m_rowHeight > 0).
    float m_rowHeight { 0.0f };
    float m_knownAspect { 0.0f };            // before a payload is held
    uint32_t m_sourceLimitedHeight { 0 };    // 0: none recorded
    bool m_sizeUpgradesDeferred { false };
    ThumbSizePolicy::UpgradeTracker m_upgrades {};   // per binding
    SourceLimitedHandler m_onSourceLimited {};

    // The CPU payload shown (or about to be uploaded): the tile's only copy,
    // kept for device-loss and target recovery, pinned in the cache.
    ThumbPayloadCache::Pin m_payload {};
    bool m_needsCpuReupload { false };
    // m_payload is a decode not yet presented: an upload failure is recorded
    // as the item's failure instead of requesting it again.
    bool m_payloadFromDecode { false };
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
