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

HRESULT ImageBrowserThumbImage::SetSourceFile(const std::wstring& filePath)
{
    const std::wstring normalized = CommonUtil::NormalizePath(filePath);
    if (!normalized.empty() && normalized == m_filePath)
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
    m_binding.ClearFailure();
    m_binding.SetLoading(false);
    m_request.source = m_filePath;
    m_cpuPayload = {};
    m_needsCpuReupload = false;

    // Keep previous texture content while loading (avoid blank flash).
    RequestImageLoad();
    Invalidate();
    return S_OK;
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

void ImageBrowserThumbImage::RequestImageLoad()
{
    if (m_filePath.empty() || m_binding.IsLoading())
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

    m_binding.RequestLoad(m_request, m_filePath);
}

void ImageBrowserThumbImage::TryReuploadCpuPayload(ID2D1RenderTarget* target)
{
    if (!m_needsCpuReupload || !target || !m_texture)
    {
        return;
    }

    if (!m_cpuPayload.blocks || m_cpuPayload.sourcePath.empty() || m_cpuPayload.sourcePath != m_filePath)
    {
        m_needsCpuReupload = false;
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    const HRESULT hrBmp = ImageAsyncBinding::CreateD2DBitmap(
        target, m_cpuPayload, D2D1_ALPHA_MODE_IGNORE, d2dBitmap);
    if (SUCCEEDED(hrBmp) && d2dBitmap)
    {
        m_texture->SetBitmap(d2dBitmap);
        m_loadedFilePath = m_cpuPayload.sourcePath;
        m_needsCpuReupload = false;
    }
    else
    {
        m_needsCpuReupload = false;
        m_loadedFilePath.clear();
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
        m_loadedFilePath = pending.sourcePath;
        m_cpuPayload = pending;
        m_needsCpuReupload = false;
        m_binding.SetLoading(false);
    }
    else
    {
        m_binding.RecordFailure(pending.sourcePath, hrBmp);
        m_binding.SetLoading(false);
    }
}

void ImageBrowserThumbImage::DrawSelectionOverlay(ID2D1RenderTarget* target)
{
    if (!m_selected || !target)
    {
        return;
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
    if (m_selectionStyle.breatheEnabled && m_selectionStyle.breathePeriodMs > 0)
    {
        const float period = static_cast<float>(m_selectionStyle.breathePeriodMs);
        const float t = static_cast<float>(
            CommonUtil::NowMs() % static_cast<unsigned long long>(m_selectionStyle.breathePeriodMs));
        const float phase = (t / period) * 6.28318530718f;
        breathe01 = 0.5f + 0.5f * std::sinf(phase);
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
        m_selectionStyle.breatheEnabled &&
        m_selectionStyle.breathePeriodMs > 0 &&
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
    switch (reason)
    {
    case FD2D::GraphicsInvalidationReason::TargetRecreated:
    case FD2D::GraphicsInvalidationReason::DeviceLost:
    case FD2D::GraphicsInvalidationReason::RendererFallback:
    case FD2D::GraphicsInvalidationReason::Shutdown:
        m_selectionBrush.Reset();
        m_selectionShadowBrush.Reset();
        m_selectionFillBrush.Reset();
        if (m_cpuPayload.blocks && !m_cpuPayload.sourcePath.empty())
        {
            m_needsCpuReupload = true;
        }
        else if (!m_filePath.empty())
        {
            m_loadedFilePath.clear();
        }
        break;

    case FD2D::GraphicsInvalidationReason::Resize:
        break;
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
