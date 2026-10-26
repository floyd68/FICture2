#include "ImageBrowserThumbImage.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/Util.h"

#include <algorithm>
#include <cmath>

ImageBrowserThumbImage::ImageBrowserThumbImage()
    : Wnd()
{
    InitChildren();
}

ImageBrowserThumbImage::ImageBrowserThumbImage(const std::wstring& name)
    : Wnd(name)
{
    InitChildren();
}

ImageBrowserThumbImage::~ImageBrowserThumbImage() = default;

void ImageBrowserThumbImage::InitChildren()
{
    m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
    m_request.allowGpuCompressedDDS = false;

    m_texture = std::make_shared<FD2D::Image>(L"texture");
    AddChild(m_texture);

    m_spinner = std::make_shared<FD2D::Spinner>(L"loadingSpinner");
    m_spinner->SetActive(false);
    AddChild(m_spinner);

    SyncTextureDrawState();
}

void ImageBrowserThumbImage::OnAttached(FD2D::Backplate& backplate)
{
    Wnd::OnAttached(backplate);
    m_binding.SetRedrawToken(backplate.GetAsyncRedrawToken());
}

void ImageBrowserThumbImage::SyncTextureDrawState()
{
    if (!m_texture)
    {
        return;
    }

    FD2D::Image::DrawState ds {};
    ds.zoomScale = 1.0f;
    ds.panX = 0.0f;
    ds.panY = 0.0f;
    ds.rotationQuarters = 0;
    ds.highQualitySampling = true;
    ds.alphaCheckerboardEnabled = false;
    m_texture->SetDrawState(ds);
}

FD2D::Size ImageBrowserThumbImage::Measure(FD2D::Size available)
{
    if (m_request.targetSize.w > 0.0f && m_request.targetSize.h > 0.0f)
    {
        float width = m_request.targetSize.w;
        float height = m_request.targetSize.h;

        if (available.w > 0.0f)
        {
            width = (std::min)(width, available.w);
        }
        if (available.h > 0.0f)
        {
            height = (std::min)(height, available.h);
        }

        m_desired = { width, height };
        return m_desired;
    }

    m_desired = { 128.0f, 128.0f };
    return m_desired;
}

void ImageBrowserThumbImage::Arrange(FD2D::Rect finalRect)
{
    Wnd::Arrange(finalRect);
}

void ImageBrowserThumbImage::SetThumbnailSize(const FD2D::Size& size)
{
    m_request.targetSize = ImageCore::Size { size.w, size.h };
    m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
}

bool ImageBrowserThumbImage::SameListing(const ImageSourceIdentity& a, const ImageSourceIdentity& b)
{
    // Field-wise: two unknown identities are the same listing here (operator==
    // never matches an unknown identity).
    return a.known == b.known && a.size == b.size && a.writeTime == b.writeTime;
}

HRESULT ImageBrowserThumbImage::SetSourceFile(const std::wstring& filePath, const ImageSourceIdentity& identity)
{
    const std::wstring normalized = CommonUtil::NormalizePath(filePath);
    const bool samePath = !normalized.empty() && normalized == m_filePath;
    if (samePath && SameListing(identity, m_identity))
    {
        const bool hadFailure = m_binding.ClearFailureIfMatches(normalized);

        const bool hasContent = m_texture &&
            m_texture->ContentPixelSize().width > 0 &&
            m_texture->ContentPixelSize().height > 0;

        if (hadFailure || (!m_binding.IsLoading() && !hasContent))
        {
            m_request.source = m_filePath;
            RequestImageLoad();
            Invalidate();
            return S_OK;
        }

        return S_FALSE;
    }

    m_binding.Cancel();

    m_filePath = normalized;
    m_identity = identity;
    m_binding.ClearFailure();
    m_binding.MarkSettled();
    m_request.source = m_filePath;
    m_payload.Release();
    m_needsCpuReupload = false;
    if (samePath)
    {
        // The file was replaced: what is shown is the old file's thumbnail.
        m_loadedFilePath.clear();
    }

    // An item decoded before (by this or an earlier tile) is shown from its
    // cached payload; only the bitmap is rebuilt, on the next render.
    if (AdoptCachedPayload())
    {
        Invalidate();
        return S_OK;
    }

    // Keep previous texture content while loading (avoid blank flash).
    RequestImageLoad();
    Invalidate();
    return S_OK;
}

void ImageBrowserThumbImage::SetSourceIdentity(const ImageSourceIdentity& identity)
{
    if (m_filePath.empty() || SameListing(identity, m_identity))
    {
        return;
    }
    (void)SetSourceFile(m_filePath, identity);
}

void ImageBrowserThumbImage::Recycle()
{
    // A payload decoded while the tile was never rendered (an offscreen
    // prefetch tile) is cached, unpinned, instead of being dropped with the
    // request.
    if (m_payloadCache)
    {
        const ImageAsyncBinding::Payload staged = m_binding.TakePending();
        if (!m_filePath.empty() && staged.sourcePath == m_filePath)
        {
            (void)StorePayload(staged);
        }
    }
    m_binding.Cancel();
    m_binding.ClearFailure();
    m_filePath.clear();
    m_identity = {};
    m_loadedFilePath.clear();
    m_request.source.clear();
    m_requestedBox = { 0.0f, 0.0f };
    // The payload becomes evictable, and nothing of this item stays on the tile.
    m_payload.Release();
    m_needsCpuReupload = false;
    if (m_texture)
    {
        m_texture->Clear();
    }
    m_bitmapStamp = {};
    m_selected = false;
    m_selectionAnimStartMs = 0;
    if (m_spinner)
    {
        m_spinner->SetActive(false);
    }
}

void ImageBrowserThumbImage::SetSelected(bool selected)
{
    if (m_selected == selected)
    {
        return;
    }
    m_selected = selected;
    m_selectionAnimStartMs = CommonUtil::NowMs();
    Invalidate();
    if (BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }
}

void ImageBrowserThumbImage::SetSelectionStyle(const FD2D::SelectionStyle& style)
{
    m_selectionStyle = style;
    m_selectionBrush.Reset();
    m_selectionShadowBrush.Reset();
    m_selectionFillBrush.Reset();
    Invalidate();
}

void ImageBrowserThumbImage::SetOnClick(ClickHandler handler)
{
    m_onClick = std::move(handler);
}

void ImageBrowserThumbImage::SetLoadPriority(ImageCore::RequestPriority priority)
{
    if (m_loadPriority == priority)
    {
        return;
    }
    m_loadPriority = priority;
    m_binding.SetPriority(priority);
}

void ImageBrowserThumbImage::SetLoadingSpinnerEnabled(bool enabled)
{
    if (m_loadingSpinnerEnabled == enabled)
    {
        return;
    }
    m_loadingSpinnerEnabled = enabled;
    Invalidate();
}

D2D1_SIZE_F ImageBrowserThumbImage::GetBitmapSize() const
{
    if (m_texture)
    {
        const D2D1_SIZE_U px = m_texture->ContentPixelSize();
        if (px.width > 0 && px.height > 0)
        {
            return D2D1::SizeF(static_cast<float>(px.width), static_cast<float>(px.height));
        }
    }
    return D2D1::SizeF(0.0f, 0.0f);
}

bool ImageBrowserThumbImage::AdoptCachedPayload()
{
    if (!m_payloadCache || !ThumbPayloadCache::IsCacheable(m_identity))
    {
        return false;
    }
    // Looked up by the identity the item was listed with: no file access.
    ThumbPayloadCache::Pin cached = m_payloadCache->AcquireLargest(
        m_filePath, m_identity, ThumbDecodeOptions::Of(m_request));
    if (!cached)
    {
        return false;
    }
    m_payload = std::move(cached);
    m_needsCpuReupload = true;
    return true;
}

ThumbPayloadCache::Pin ImageBrowserThumbImage::StorePayload(const ImageAsyncBinding::Payload& payload)
{
    if (!m_payloadCache)
    {
        return ThumbPayloadCache::Pin::Detached(payload);
    }
    return m_payloadCache->Insert(
        payload.sourcePath, m_identity, ThumbDecodeOptions::Of(m_request), m_requestedBox, payload);
}

void ImageBrowserThumbImage::RequestImageLoad()
{
    if (m_filePath.empty() || m_binding.IsLoading())
    {
        return;
    }

    // A cached payload waits for its upload (deferred during a size move).
    if (m_needsCpuReupload && m_payload && m_payload.Get().sourcePath == m_filePath)
    {
        return;
    }

    if (m_binding.IsFailedFor(m_filePath))
    {
        return;
    }

    if (m_loadedFilePath == m_filePath)
    {
        const D2D1_SIZE_F sz = GetBitmapSize();
        if (sz.width > 0.0f && sz.height > 0.0f)
        {
            return;
        }
    }

    m_request.source = m_filePath;
    m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
    m_request.allowGpuCompressedDDS = false;
    m_requestedBox = m_request.targetSize;

    m_binding.RequestLoad(m_request, m_filePath, m_loadPriority);
}

void ImageBrowserThumbImage::TryReuploadCpuPayload(ID2D1RenderTarget* target)
{
    if (!m_needsCpuReupload || !target || !m_texture)
    {
        return;
    }

    const ImageAsyncBinding::Payload& payload = m_payload.Get();
    if (!payload.blocks || payload.sourcePath.empty() || payload.sourcePath != m_filePath)
    {
        m_needsCpuReupload = false;
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    const HRESULT hrBmp = ImageAsyncBinding::CreateD2DBitmap(
        target, payload, D2D1_ALPHA_MODE_IGNORE, d2dBitmap);
    if (SUCCEEDED(hrBmp) && d2dBitmap)
    {
        m_texture->SetBitmap(d2dBitmap);
        m_bitmapStamp = CurrentTargetStamp();
        m_loadedFilePath = payload.sourcePath;
        m_needsCpuReupload = false;
    }
    else
    {
        // A new decode replaces it (RequestImageLoad on the next render).
        m_needsCpuReupload = false;
        m_loadedFilePath.clear();
        m_payload.Release();
    }
}

void ImageBrowserThumbImage::ApplyPendingPayload(ID2D1RenderTarget* target)
{
    if (!target)
    {
        return;
    }

    const ImageAsyncBinding::Payload pending = m_binding.TakePending();
    if (!pending.blocks || pending.sourcePath.empty() ||
        pending.width == 0 || pending.height == 0 || pending.rowPitch == 0)
    {
        return;
    }

    // Path gate (fixes FD2D::ThumbImage gap that applied stale pending without checking path).
    if (pending.sourcePath != m_filePath)
    {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    const HRESULT hrBmp = ImageAsyncBinding::CreateD2DBitmap(
        target, pending, D2D1_ALPHA_MODE_IGNORE, d2dBitmap);

    if (SUCCEEDED(hrBmp) && d2dBitmap)
    {
        if (m_texture)
        {
            m_texture->SetBitmap(d2dBitmap);
        }
        m_bitmapStamp = CurrentTargetStamp();
        m_loadedFilePath = pending.sourcePath;
        // The cache entry is the tile's CPU copy, pinned while it is shown.
        m_payload = StorePayload(pending);
        m_needsCpuReupload = false;
        m_binding.MarkSettled();
    }
    else
    {
        m_binding.RecordFailure(pending.sourcePath, hrBmp);
        m_binding.MarkSettled();
    }
}

void ImageBrowserThumbImage::DrawSelectionOverlay(ID2D1RenderTarget* target)
{
    if (!m_selected || !target)
    {
        return;
    }

    if (RefreshTargetResourceStamp(m_brushStamp))
    {
        m_selectionBrush.Reset();
        m_selectionShadowBrush.Reset();
        m_selectionFillBrush.Reset();
    }
    if (!m_selectionBrush)
    {
        (void)target->CreateSolidColorBrush(m_selectionStyle.accent, &m_selectionBrush);
    }
    if (!m_selectionShadowBrush)
    {
        (void)target->CreateSolidColorBrush(m_selectionStyle.shadow, &m_selectionShadowBrush);
    }
    if (!m_selectionFillBrush)
    {
        (void)target->CreateSolidColorBrush(
            D2D1::ColorF(m_selectionStyle.fill.r, m_selectionStyle.fill.g, m_selectionStyle.fill.b, 0.0f),
            &m_selectionFillBrush);
    }

    if (!m_selectionBrush)
    {
        return;
    }

    D2D1_RECT_F r = LayoutRect();
    const D2D1_SIZE_F bitmapSize = GetBitmapSize();
    if (bitmapSize.width > 0.0f && bitmapSize.height > 0.0f)
    {
        r = FD2D::Util::ComputeAspectFitRect(r, bitmapSize);
    }

    float selT = 1.0f;
    bool selAnimating = false;
    if (m_selectionAnimStartMs != 0 && m_selectionAnimMs > 0)
    {
        const unsigned long long elapsed = CommonUtil::NowMs() - m_selectionAnimStartMs;
        selT = CommonUtil::Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_selectionAnimMs));
        selAnimating = selT < 1.0f;
    }

    const float ease = 1.0f - (1.0f - selT) * (1.0f - selT);
    const float popInflate = m_selectionStyle.popInflate * (1.0f - ease);
    const float baseInflate = m_selectionStyle.baseInflate;

    float breathe01 = 0.0f;
    bool breatheRunning = false;
    if (m_selectionStyle.breatheEnabled && m_selectionAnimStartMs != 0)
    {
        breathe01 = FD2D::SelectionBreathe(
            CommonUtil::NowMs() - m_selectionAnimStartMs,
            m_selectionStyle.breathePeriodMs,
            m_selectionStyle.breatheCycles,
            breatheRunning);
    }
    const float breatheInflate = m_selectionStyle.breatheInflateAmp * breathe01;

    r.left -= (baseInflate + popInflate + breatheInflate);
    r.top -= (baseInflate + popInflate + breatheInflate);
    r.right += (baseInflate + popInflate + breatheInflate);
    r.bottom += (baseInflate + popInflate + breatheInflate);

    const float radius = m_selectionStyle.radius;
    const D2D1_ROUNDED_RECT rr { r, radius, radius };

    if (m_selectionFillBrush)
    {
        const float fillA = m_selectionStyle.fillMaxAlpha * ease;
        m_selectionFillBrush->SetColor(
            D2D1::ColorF(m_selectionStyle.fill.r, m_selectionStyle.fill.g, m_selectionStyle.fill.b, fillA));
        const D2D1_ROUNDED_RECT fillRR {
            r,
            (std::max)(0.0f, radius - 1.0f),
            (std::max)(0.0f, radius - 1.0f)
        };
        target->FillRoundedRectangle(fillRR, m_selectionFillBrush.Get());
    }

    const float shadowW = m_selectionStyle.shadowThickness;
    const float accentW = (std::max)(
        0.0f,
        m_selectionStyle.accentThickness + (1.0f - ease) + (m_selectionStyle.breatheThicknessAmp * breathe01));

    if (m_selectionBrush)
    {
        const float baseA = m_selectionStyle.accent.a;
        const float pulseA =
            baseA * (1.0f - m_selectionStyle.breatheAlphaAmp) +
            baseA * m_selectionStyle.breatheAlphaAmp * breathe01;
        m_selectionBrush->SetColor(
            D2D1::ColorF(m_selectionStyle.accent.r, m_selectionStyle.accent.g, m_selectionStyle.accent.b, pulseA));
    }

    if (m_selectionShadowBrush)
    {
        target->DrawRoundedRectangle(rr, m_selectionShadowBrush.Get(), shadowW);
    }
    target->DrawRoundedRectangle(rr, m_selectionBrush.Get(), accentW);

    const bool breatheAnimActive =
        breatheRunning &&
        (
            m_selectionStyle.breatheInflateAmp > 0.0f ||
            m_selectionStyle.breatheThicknessAmp > 0.0f ||
            m_selectionStyle.breatheAlphaAmp > 0.0f
        );

    if ((selAnimating || breatheAnimActive) && BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }
}

void ImageBrowserThumbImage::OnGraphicsInvalidated(
    FD2D::GraphicsInvalidationReason reason,
    const FD2D::GraphicsGeneration& generation)
{
    // The presented bitmap belongs to the render target: rebuild it only when
    // the target generation it was created under changed (brushes are checked
    // at render time through m_brushStamp).
    const bool bitmapLost =
        reason != FD2D::GraphicsInvalidationReason::Shutdown &&
        !m_loadedFilePath.empty() &&
        !(m_bitmapStamp == CurrentTargetStamp());
    if (bitmapLost)
    {
        // CPU payloads do not depend on graphics generations: the pinned one
        // rebuilds the bitmap, and no cache entry is dropped.
        if (m_payload && !m_payload.Get().sourcePath.empty())
        {
            m_needsCpuReupload = true;
        }
        else
        {
            m_loadedFilePath.clear();
        }
    }

    Wnd::OnGraphicsInvalidated(reason, generation);
}

void ImageBrowserThumbImage::OnRender(ID2D1RenderTarget* target)
{
    if (!target)
    {
        return;
    }

    FD2D::Backplate* bp = BackplateRef();
    const bool deferBitmapUpload = (bp != nullptr && bp->IsInSizeMove());

    if (!deferBitmapUpload)
    {
        ApplyPendingPayload(target);
        TryReuploadCpuPayload(target);
    }

    const bool hasContent = GetBitmapSize().width > 0.0f;
    if (!hasContent && !m_binding.IsLoading())
    {
        RequestImageLoad();
    }

    SyncTextureDrawState();

    const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_binding.IsLoading();
    if (m_spinner)
    {
        m_spinner->SetActive(shouldShowSpinner);
    }

    // Texture then selection then spinner (match FD2D::ThumbImage z-order).
    if (m_texture)
    {
        m_texture->OnRender(target);
    }
    DrawSelectionOverlay(target);
    if (m_spinner)
    {
        m_spinner->OnRender(target);
    }
}

bool ImageBrowserThumbImage::OnInputEvent(const FD2D::InputEvent& event)
{
    switch (event.type)
    {
    case FD2D::InputEventType::MouseDown:
    {
        if (event.button != FD2D::MouseButton::Left || !event.hasPoint)
        {
            break;
        }
        POINT pt = event.point;
        const D2D1_RECT_F r = LayoutRect();
        if (static_cast<float>(pt.x) >= r.left &&
            static_cast<float>(pt.x) <= r.right &&
            static_cast<float>(pt.y) >= r.top &&
            static_cast<float>(pt.y) <= r.bottom)
        {
            if (m_onClick)
            {
                m_onClick();
                return true;
            }
        }
        break;
    }
    default:
        break;
    }

    return Wnd::OnInputEvent(event);
}
