#include "ImageBrowserMainImage.h"

#include "AppLog.h"
#include "CommonUtil.h"
#include "ImageAlphaPresentation.h"
#include "ImageGpuResourceCache.h"
#include "FD2D/Backplate.h"
#include "FD2D/Util.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace
{
    ImageAlphaInfo AlphaInfoFromPayload(const ImageAsyncBinding::Payload& payload)
    {
        return
        {
            payload.alphaEncoding,
            payload.alphaUsageHint,
            payload.sourceWasBlockCompressed,
        };
    }

    ImageCore::DecodedImage DecodedImageFromPayload(const ImageAsyncBinding::Payload& payload)
    {
        ImageCore::DecodedImage image {};
        image.width = payload.width;
        image.height = payload.height;
        image.dxgiFormat = payload.format;
        image.alphaEncoding = payload.alphaEncoding;
        image.alphaUsageHint = payload.alphaUsageHint;
        image.sourceWasBlockCompressed = payload.sourceWasBlockCompressed;
        image.rowPitchBytes = payload.rowPitch;
        image.blockBytes = payload.blocks ? static_cast<uint32_t>(payload.blocks->size()) : 0;
        image.blocks = payload.blocks;
        image.sourceMipLevels = payload.sourceMipLevels;
        image.sourceMipIndex = payload.sourceMipIndex;
        return image;
    }

    HRESULT CreatePremultipliedD2DBitmap(
        ID2D1RenderTarget* target,
        const ImageAsyncBinding::Payload& payload,
        ImageCore::AlphaUsage overrideUsage,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap)
    {
        const ImageAlphaInfo alpha = AlphaInfoFromPayload(payload);
        const ImageCore::AlphaUsage usage = ResolveAlphaUsage(alpha, overrideUsage);
        const ImageCore::DecodedImage decoded = DecodedImageFromPayload(payload);
        std::vector<std::uint8_t> presentation = BuildBgra8Presentation(decoded, usage);
        if (presentation.empty())
        {
            return E_FAIL;
        }

        ImageAsyncBinding::Payload present = payload;
        present.blocks = std::make_shared<std::vector<std::uint8_t>>(std::move(presentation));
        return ImageAsyncBinding::CreateD2DBitmap(
            target, present, D2D1_ALPHA_MODE_PREMULTIPLIED, outBitmap);
    }

    const wchar_t* ChannelName(int mode)
    {
        switch (mode)
        {
        case 1: return L"R";
        case 2: return L"G";
        case 3: return L"B";
        case 4: return L"A";
        default: return L"RGBA";
        }
    }

    const wchar_t* AlphaEncodingName(ImageCore::AlphaEncoding encoding)
    {
        switch (encoding)
        {
        case ImageCore::AlphaEncoding::Straight: return L"Straight";
        case ImageCore::AlphaEncoding::Premultiplied: return L"Premultiplied";
        case ImageCore::AlphaEncoding::Opaque: return L"Opaque";
        default: return L"Unknown";
        }
    }

    const wchar_t* AlphaUsageName(ImageCore::AlphaUsage usage)
    {
        switch (usage)
        {
        case ImageCore::AlphaUsage::Coverage: return L"Transparency";
        case ImageCore::AlphaUsage::Data: return L"Opaque (data)";
        case ImageCore::AlphaUsage::Auto: return L"Auto";
        default: return L"Unknown";
        }
    }

    int BitsPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 8;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 32;
        default:
            return 0;
        }
    }

    void LogImageHr(const wchar_t* stage, const std::wstring& path, HRESULT hr)
    {
        if (SUCCEEDED(hr))
        {
            return;
        }

        const std::wstring msg = path.empty()
            ? std::format(L"[ImageBrowserMainImage] {} failed: 0x{:08X}\n",
                stage, static_cast<unsigned>(hr))
            : std::format(L"[ImageBrowserMainImage] {} failed ({}): 0x{:08X}\n",
                stage, path, static_cast<unsigned>(hr));
        OutputDebugStringW(msg.c_str());
    }

    void RotateVectorByQuarters(float dx, float dy, int quarters, float& outX, float& outY)
    {
        switch (((quarters % 4) + 4) % 4)
        {
        case 1: outX = -dy; outY = dx;  break;
        case 2: outX = -dx; outY = -dy; break;
        case 3: outX = dy;  outY = -dx; break;
        default: outX = dx; outY = dy;  break;
        }
    }

    bool IsCompressedDxgiFormat(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }
}

ImageBrowserMainImage::ImageBrowserMainImage()
    : Wnd()
{
    InitChildren();
}

ImageBrowserMainImage::ImageBrowserMainImage(const std::wstring& name)
    : Wnd(name)
{
    InitChildren();
}

ImageBrowserMainImage::~ImageBrowserMainImage() = default;

void ImageBrowserMainImage::InitChildren()
{
    m_request.purpose = ImageCore::ImagePurpose::FullResolution;

    m_texture = std::make_shared<FD2D::Image>(L"texture");
    AddChild(m_texture);

    m_spinner = std::make_shared<FD2D::Spinner>(L"loadingSpinner");
    m_spinner->SetActive(false);
    AddChild(m_spinner);
}

void ImageBrowserMainImage::OnAttached(FD2D::Backplate& backplate)
{
    Wnd::OnAttached(backplate);
    m_binding.SetRedrawToken(backplate.GetAsyncRedrawToken());
}

uint64_t ImageBrowserMainImage::DeviceGeneration() const
{
    if (BackplateRef() == nullptr)
    {
        return 0;
    }
    return BackplateRef()->GetGraphicsGeneration().device;
}

void ImageBrowserMainImage::ApplyPresentation(
    PresentationKind kind,
    const std::wstring& path,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    const ImageAlphaInfo& alpha,
    uint32_t sourceMipLevels,
    uint32_t sourceMipIndex,
    const ImageSourceIdentity& identity)
{
    SetLoadedFilePath(path);
    m_loadedW = width;
    m_loadedH = height;
    m_loadedFormat = format;
    m_alpha = alpha;
    m_sourceMipLevels = (std::max)(1u, sourceMipLevels);
    m_sourceMipIndex = sourceMipIndex;
    if (sourceMipIndex == 0)
    {
        m_sourceWidth = width;
        m_sourceHeight = height;
    }
    m_presentationKind = kind;
    m_gpuPresentation = (kind == PresentationKind::ShaderResource);
    // An SRV belongs to the device, a D2D bitmap to the render target.
    m_presentationStamp = (kind == PresentationKind::ShaderResource) ? CurrentDeviceStamp() : CurrentTargetStamp();
    m_presentationIdentity = identity;
    m_needsCpuReupload = false;
    m_selectingMip = false;
    SyncTextureDrawState();
}

void ImageBrowserMainImage::ResetPresentation()
{
    m_loadedW = 0;
    m_loadedH = 0;
    m_loadedFormat = DXGI_FORMAT_UNKNOWN;
    m_alpha = {};
    m_sourceMipLevels = 1;
    m_sourceMipIndex = 0;
    m_sourceWidth = 0;
    m_sourceHeight = 0;
    m_gpuPresentation = false;
    m_presentationKind = PresentationKind::None;
    m_presentationStamp = {};
    m_presentationIdentity = {};
}

HRESULT ImageBrowserMainImage::CreateBgraShaderResource(
    const ImageAsyncBinding::Payload& payload,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) const
{
    ID3D11Device* dev = BackplateRef() != nullptr ? BackplateRef()->D3DDevice() : nullptr;
    if (dev == nullptr || !payload.blocks || payload.blocks->empty() || !ImageAsyncBinding::IsCpuBgra8Format(payload.format))
    {
        return E_NOINTERFACE;
    }

    D3D11_TEXTURE2D_DESC td {};
    td.Width = payload.width;
    td.Height = payload.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init {};
    init.pSysMem = payload.blocks->data();
    init.SysMemPitch = payload.rowPitch ? payload.rowPitch : payload.width * 4u;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr) || !tex)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = dev->CreateShaderResourceView(tex.Get(), nullptr, &outSrv);
    return (SUCCEEDED(hr) && outSrv) ? S_OK : (FAILED(hr) ? hr : E_FAIL);
}

void ImageBrowserMainImage::ReloadSource()
{
    if (m_filePath.empty())
    {
        return;
    }

    // Everything is reset before Invalidate(), which may render synchronously
    // and dispatch the reload request right away: that request must be mip 0.

    ImageGpuResourceCache::Instance().InvalidatePath(m_filePath);
    m_binding.Cancel();
    m_binding.ClearFailure();
    m_stagedCacheHit.reset();
    m_forceCpuDecode.store(false);
    m_cpuPayload = {};
    m_needsCpuReupload = false;
    m_selectingMip = false;
    m_request.mipLevel = 0;
    // The old pixels stay on screen until the new decode arrives, but they are no
    // longer "the current content", so the next render requests a decode.
    SetLoadedFilePath({});
    ResetPresentation();
    Invalidate();
}

FD2D::Size ImageBrowserMainImage::Measure(FD2D::Size available)
{
    if (available.w > 0.0f && available.h > 0.0f)
    {
        m_desired = available;
    }
    else
    {
        m_desired = { 800.0f, 600.0f };
    }
    return m_desired;
}

void ImageBrowserMainImage::Arrange(FD2D::Rect finalRect)
{
    Wnd::Arrange(finalRect);
}

void ImageBrowserMainImage::SyncTextureDrawState()
{
    if (!m_texture)
    {
        return;
    }

    FD2D::Image::DrawState ds {};
    ds.zoomScale = m_zoomScale;
    ds.panX = m_panX;
    ds.panY = m_panY;
    ds.rotationQuarters = m_rotationQuarters;
    ds.highQualitySampling = m_highQualitySampling;
    ds.alphaCheckerboardEnabled = m_alphaCheckerboardEnabled;
    ds.channelMode = m_channelMode;
    ds.sourceAlphaEncoding =
        m_alpha.encoding == ImageCore::AlphaEncoding::Premultiplied ? 1 : 0;
    ds.sourceAlphaUsage =
        ResolveAlphaUsage(m_alpha, m_alphaUsageOverride) == ImageCore::AlphaUsage::Data
            ? 1
            : 0;
    m_texture->SetDrawState(ds);
    // Draw state and loaded metadata only change through here or ApplyPresentation
    // (which ends here too): report information changes, not every frame.
    NotifyInfoIfChanged();
}

bool ImageBrowserMainImage::TryGetContentSize(D2D1_SIZE_F& outSize) const
{
    if (m_loadedW > 0 && m_loadedH > 0)
    {
        outSize = { static_cast<float>(m_loadedW), static_cast<float>(m_loadedH) };
        return true;
    }

    if (m_texture)
    {
        const D2D1_SIZE_U px = m_texture->ContentPixelSize();
        if (px.width > 0 && px.height > 0)
        {
            outSize = { static_cast<float>(px.width), static_cast<float>(px.height) };
            return true;
        }
    }

    return false;
}

bool ImageBrowserMainImage::HasDisplayedContentForCurrentPath() const
{
    if (m_loadedFilePath != m_filePath || m_filePath.empty())
    {
        return false;
    }

    if (m_sourceMipIndex != m_request.mipLevel)
    {
        return false;
    }

    if (!m_texture)
    {
        return false;
    }

    const D2D1_SIZE_U px = m_texture->ContentPixelSize();
    return px.width > 0 && px.height > 0;
}

void ImageBrowserMainImage::ClampPanToVisible()
{
    D2D1_SIZE_F contentSize {};
    if (!TryGetContentSize(contentSize))
    {
        return;
    }

    const D2D1_RECT_F layoutRect = LayoutRect();
    const float layoutWidth = layoutRect.right - layoutRect.left;
    const float layoutHeight = layoutRect.bottom - layoutRect.top;
    if (!(layoutWidth > 0.0f && layoutHeight > 0.0f && contentSize.width > 0.0f && contentSize.height > 0.0f))
    {
        return;
    }

    const D2D1_RECT_F baseRect = FD2D::Util::ComputeAspectFitRect(layoutRect, contentSize);
    const float width = baseRect.right - baseRect.left;
    const float height = baseRect.bottom - baseRect.top;
    if (width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    const float scaledWidth = width * m_zoomScale;
    const float scaledHeight = height * m_zoomScale;
    if (scaledWidth <= 0.0f || scaledHeight <= 0.0f)
    {
        return;
    }

    const float centerX = (baseRect.left + baseRect.right) * 0.5f;
    const float centerY = (baseRect.top + baseRect.bottom) * 0.5f;
    constexpr float kMinVisible = 1.0f;

    float minPanX = (layoutRect.left + kMinVisible) - (centerX + scaledWidth * 0.5f);
    float maxPanX = (layoutRect.right - kMinVisible) - (centerX - scaledWidth * 0.5f);
    if (minPanX > maxPanX)
    {
        const float mid = (minPanX + maxPanX) * 0.5f;
        minPanX = mid;
        maxPanX = mid;
    }
    m_panX = (std::max)(minPanX, (std::min)(maxPanX, m_panX));

    float minPanY = (layoutRect.top + kMinVisible) - (centerY + scaledHeight * 0.5f);
    float maxPanY = (layoutRect.bottom - kMinVisible) - (centerY - scaledHeight * 0.5f);
    if (minPanY > maxPanY)
    {
        const float mid = (minPanY + maxPanY) * 0.5f;
        minPanY = mid;
        maxPanY = mid;
    }
    m_panY = (std::max)(minPanY, (std::min)(maxPanY, m_panY));
}

ImageViewTransform ImageBrowserMainImage::GetViewTransform() const
{
    ImageViewTransform vt {};
    vt.zoomScale = m_zoomScale;
    vt.targetZoomScale = m_targetZoomScale;
    vt.zoomVelocity = m_zoomVelocity;
    vt.panX = m_panX;
    vt.panY = m_panY;
    vt.rotationQuarters = m_rotationQuarters;
    vt.channelMode = m_channelMode;
    vt.mipLevel = m_sourceMipIndex;
    return vt;
}

ImageLoadedInfo ImageBrowserMainImage::GetLoadedInfo() const
{
    ImageLoadedInfo info {};
    info.width = m_loadedW;
    info.height = m_loadedH;
    info.format = m_loadedFormat;
    info.sourceMipLevels = (std::max)(1u, m_sourceMipLevels);
    info.sourceMipIndex = m_sourceMipIndex;
    info.sourceWidth = m_sourceWidth;
    info.sourceHeight = m_sourceHeight;
    info.alphaEncoding = m_alpha.encoding;
    info.alphaUsageHint = m_alpha.usageHint;
    info.alphaUsageOverride = m_alphaUsageOverride;
    info.effectiveAlphaUsage = EffectiveAlphaUsage();
    info.sourceWasBlockCompressed = m_alpha.sourceWasBlockCompressed;
    info.gpuPresentation = m_gpuPresentation;
    info.channelMode = m_channelMode;
    info.alphaCheckerboardEnabled = m_alphaCheckerboardEnabled;
    info.highQualitySampling = m_highQualitySampling;
    info.zoomScale = m_zoomScale;
    info.panX = m_panX;
    info.panY = m_panY;
    info.rotationQuarters = m_rotationQuarters;
    info.sourcePath = m_loadedFilePath;
    return info;
}

void ImageBrowserMainImage::SetViewTransform(const ImageViewTransform& vt, bool notify)
{
    m_zoomScale = vt.zoomScale;
    m_targetZoomScale = vt.targetZoomScale;
    m_zoomVelocity = vt.zoomVelocity;
    m_panX = vt.panX;
    m_panY = vt.panY;
    m_rotationQuarters = ((vt.rotationQuarters % 4) + 4) % 4;
    m_channelMode = (std::max)(0, (std::min)(4, vt.channelMode));

    m_panning = false;
    m_panArmed = false;
    m_pointerZoomActive = false;
    m_lastZoomAnimMs = CommonUtil::NowMs();

    const uint32_t maxMip = (std::max)(1u, m_sourceMipLevels) - 1u;
    const uint32_t desiredMip = (std::min)(vt.mipLevel, maxMip);
    if (desiredMip != m_sourceMipIndex || desiredMip != m_request.mipLevel)
    {
        const bool prevSuppress = m_suppressViewNotify;
        m_suppressViewNotify = true;
        SelectMip(desiredMip);
        m_suppressViewNotify = prevSuppress;
    }

    ClampPanToVisible();
    SyncTextureDrawState();

    const bool prevSuppress = m_suppressViewNotify;
    if (!notify)
    {
        m_suppressViewNotify = true;
    }

    Invalidate();

    if (notify && !m_suppressViewNotify && m_onViewChanged)
    {
        m_onViewChanged(GetViewTransform());
    }

    m_suppressViewNotify = prevSuppress;
}

void ImageBrowserMainImage::SetOnViewChanged(ViewChangedHandler handler)
{
    m_onViewChanged = std::move(handler);
}

void ImageBrowserMainImage::SetOnPresentedPathChanged(PresentedPathHandler handler)
{
    m_onPresentedPathChanged = std::move(handler);
}

void ImageBrowserMainImage::SetOnInfoChanged(InfoChangedHandler handler)
{
    m_onInfoChanged = std::move(handler);
}

void ImageBrowserMainImage::NotifyInfoIfChanged()
{
    if (!m_onInfoChanged)
    {
        return;
    }

    const ImageLoadedInfo li = GetLoadedInfo();
    const int zoomPercent = static_cast<int>(std::round(li.zoomScale * 100.0f));
    std::wstring key = li.sourcePath;
    for (const uint64_t v : {
             static_cast<uint64_t>(li.width), static_cast<uint64_t>(li.height),
             static_cast<uint64_t>(li.format), static_cast<uint64_t>(li.sourceMipLevels),
             static_cast<uint64_t>(li.sourceMipIndex), static_cast<uint64_t>(li.sourceWidth),
             static_cast<uint64_t>(li.sourceHeight), static_cast<uint64_t>(li.alphaEncoding),
             static_cast<uint64_t>(li.alphaUsageHint), static_cast<uint64_t>(li.alphaUsageOverride),
             static_cast<uint64_t>(li.effectiveAlphaUsage), static_cast<uint64_t>(li.sourceWasBlockCompressed),
             static_cast<uint64_t>(li.gpuPresentation), static_cast<uint64_t>(li.channelMode),
             static_cast<uint64_t>(li.alphaCheckerboardEnabled), static_cast<uint64_t>(li.highQualitySampling),
             static_cast<uint64_t>(static_cast<int64_t>(zoomPercent)), static_cast<uint64_t>(li.rotationQuarters) })
    {
        key += L'|';
        key += std::to_wstring(v);
    }
    if (key == m_lastInfoKey)
    {
        return;
    }
    m_lastInfoKey = std::move(key);
    m_onInfoChanged();
}

void ImageBrowserMainImage::SetLoadedFilePath(const std::wstring& path)
{
    if (path == m_loadedFilePath)
    {
        return;
    }
    m_loadedFilePath = path;
    if (m_onPresentedPathChanged)
    {
        m_onPresentedPathChanged(m_loadedFilePath);
    }
}

void ImageBrowserMainImage::RotateCW()
{
    auto vt = GetViewTransform();
    vt.rotationQuarters = (vt.rotationQuarters + 1) % 4;
    SetViewTransform(vt, true);
}

void ImageBrowserMainImage::RotateCCW()
{
    auto vt = GetViewTransform();
    vt.rotationQuarters = (vt.rotationQuarters + 3) % 4;
    SetViewTransform(vt, true);
}

void ImageBrowserMainImage::SetChannelMode(int mode)
{
    const int clamped = (std::max)(0, (std::min)(4, mode));
    m_channelMode = (m_channelMode == clamped) ? 0 : clamped;
    SyncTextureDrawState();
    Invalidate();
    if (!m_suppressViewNotify && m_onViewChanged)
    {
        m_onViewChanged(GetViewTransform());
    }
}

void ImageBrowserMainImage::SetAlphaUsageOverride(ImageCore::AlphaUsage usage)
{
    if (m_alphaUsageOverride == usage)
    {
        return;
    }

    m_alphaUsageOverride = usage;
    SyncTextureDrawState();
    if (!m_gpuPresentation && m_cpuPayload.blocks && !m_cpuPayload.sourcePath.empty())
    {
        m_needsCpuReupload = true;
    }
    Invalidate();
    if (!m_suppressViewNotify && m_onViewChanged)
    {
        m_onViewChanged(GetViewTransform());
    }
}

ImageCore::AlphaUsage ImageBrowserMainImage::EffectiveAlphaUsage() const
{
    return ResolveAlphaUsage(m_alpha, m_alphaUsageOverride);
}

std::wstring ImageBrowserMainImage::InformationText() const
{
    if (m_loadedFilePath.empty() || m_loadedW == 0 || m_loadedH == 0)
    {
        return L"No image is loaded.";
    }

    const int bpp = BitsPerPixel(m_loadedFormat);
    std::wstring text =
        L"Path: " + m_loadedFilePath +
        L"\n\nCurrent dimensions: " +
        std::to_wstring(m_loadedW) + L" \u00d7 " + std::to_wstring(m_loadedH) +
        L"\nSource dimensions: " +
        std::to_wstring(m_sourceWidth) + L" \u00d7 " + std::to_wstring(m_sourceHeight) +
        L"\nPixel format: DXGI " + std::to_wstring(static_cast<unsigned>(m_loadedFormat)) +
        L"\nEffective bits per pixel: " +
        (bpp > 0 ? std::to_wstring(bpp) : L"Unknown") +
        L"\nMip level: " +
        std::to_wstring(m_sourceMipIndex) + L" / " +
        std::to_wstring((std::max)(1u, m_sourceMipLevels) - 1u) +
        L" (" + std::to_wstring((std::max)(1u, m_sourceMipLevels)) + L" levels)" +
        L"\nSource compression: " +
        (m_alpha.sourceWasBlockCompressed ? L"Block compressed" : L"Uncompressed") +
        L"\nPresentation: " +
        (m_gpuPresentation ? L"D3D11 texture" : L"D2D bitmap") +
        L"\n\nAlpha encoding: " + AlphaEncodingName(m_alpha.encoding) +
        L"\nAlpha decoder hint: " + AlphaUsageName(m_alpha.usageHint) +
        L"\nAlpha override: " + AlphaUsageName(m_alphaUsageOverride) +
        L"\nEffective alpha usage: " + AlphaUsageName(EffectiveAlphaUsage()) +
        L"\n\nChannel: " + ChannelName(m_channelMode) +
        L"\nCheckerboard: " + (m_alphaCheckerboardEnabled ? L"On" : L"Off") +
        L"\nZoom: " +
        std::to_wstring(static_cast<int>(std::lround(m_zoomScale * 100.0f))) +
        L"%\nRotation: " +
        std::to_wstring((((m_rotationQuarters % 4) + 4) % 4) * 90) +
        L"\u00b0\nPan: " +
        std::to_wstring(static_cast<int>(std::lround(m_panX))) + L", " +
        std::to_wstring(static_cast<int>(std::lround(m_panY))) +
        L" px\nSampling: " +
        (m_highQualitySampling
            ? (m_gpuPresentation ? L"D3D11 anisotropic" : L"D2D smooth")
            : (m_gpuPresentation ? L"D3D11 point" : L"D2D nearest"));
    return text;
}

bool ImageBrowserMainImage::SelectMip(uint32_t mipLevel)
{
    if (m_filePath.empty())
    {
        return false;
    }

    // The previous source is still shown while the requested one loads: its
    // mips are not the requested source's.
    if (m_presentationKind != PresentationKind::None && m_loadedFilePath != m_filePath)
    {
        return false;
    }

    const uint32_t levels = (std::max)(1u, m_sourceMipLevels);
    if (mipLevel >= levels)
    {
        return false;
    }

    // Validate against the file as it is now, not the content already shown:
    // if it (or its host archive) was replaced, every cached mip is stale.
    // Reload mip 0 first - only it supplies the new source size and mip count -
    // and select the requested mip once that has been presented. The pending
    // mip is recorded before ReloadSource(), whose Invalidate() may render.
    const ImageSourceIdentity current = ImageSourceIdentity::ForPath(m_filePath);
    if (m_presentationKind != PresentationKind::None && current != m_presentationIdentity)
    {
        m_mipAfterReload = mipLevel;
        ReloadSource();
        return true;
    }
    m_mipAfterReload = 0;

    if (mipLevel == m_sourceMipIndex && HasDisplayedContentForCurrentPath())
    {
        return true;
    }

    m_request.mipLevel = mipLevel;
    m_selectingMip = true;
    m_binding.Cancel();
    m_binding.ClearFailure();
    m_stagedCacheHit.reset();
    m_forceCpuDecode.store(false);

    CachedPresentation cached {};
    if (TryGetCachedPresentation(m_filePath, mipLevel, current, cached))
    {
        m_binding.MarkSettled();
        PresentCached(std::move(cached));
        Invalidate();
        if (!m_suppressViewNotify && m_onViewChanged)
        {
            m_onViewChanged(GetViewTransform());
        }
        return true;
    }

    m_binding.MarkSettled();
    RequestImageLoad();
    Invalidate();
    return true;
}

uint32_t ImageBrowserMainImage::MipLevel() const
{
    return m_sourceMipIndex;
}

uint32_t ImageBrowserMainImage::MipLevels() const
{
    return (std::max)(1u, m_sourceMipLevels);
}

HRESULT ImageBrowserMainImage::SetSourceFile(const std::wstring& filePath)
{
    const std::wstring normalized = CommonUtil::NormalizePath(filePath);

    if (!normalized.empty() && normalized != m_filePath)
    {
        if (m_panning)
        {
            m_panning = false;
            if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
            {
                if (GetCapture() == BackplateRef()->Window())
                {
                    ReleaseCapture();
                }
            }
        }

        m_rotationQuarters = 0;
        m_pointerZoomActive = false;
        m_zoomVelocity = 0.0f;
        m_lastZoomAnimMs = CommonUtil::NowMs();
    }

    if (!normalized.empty() && normalized == m_filePath)
    {
        const ImageSourceIdentity current = ImageSourceIdentity::ForPath(normalized);

        // The file (or its host archive) was replaced since it was decoded:
        // reload instead of keeping the stale presentation. Only the shown
        // presentation of this path is checked: a previous source may still be
        // shown while this one loads.
        if (m_presentationKind != PresentationKind::None &&
            m_loadedFilePath == normalized &&
            m_presentationIdentity != current)
        {
            ReloadSource();
            return S_OK;
        }

        // This path is still loading (decode in flight or staged), and the file
        // changed since that decode was requested: restart the decode. The
        // previous source stays on screen until the new result is handed off.
        const bool requestIdentityChanged = (m_requestIdentity.known || current.known) &&
            m_requestIdentity != current;
        if (m_loadedFilePath != normalized && m_binding.IsLoading() && requestIdentityChanged)
        {
            ImageGpuResourceCache::Instance().InvalidatePath(normalized);
            m_binding.Cancel();
            m_binding.MarkSettled();
            m_stagedCacheHit.reset();
            Invalidate();   // the next render requests the decode again
            return S_OK;
        }

        if (!m_binding.ClearFailureIfMatches(normalized, true /*requireFailedHr*/))
        {
            return S_FALSE;
        }

        if (m_loadedFilePath == m_filePath && HasDisplayedContentForCurrentPath())
        {
            return S_FALSE;
        }
    }

    // Supersede the in-flight request (cancel its handle, drop any staged payload).
    m_binding.Cancel();
    m_stagedCacheHit.reset();

    // Only the request changes here. The presentation on screen - its content,
    // loaded metadata, presented path and CPU payload - stays as one unit until
    // the frame handoff replaces it (PresentPendingContent).
    m_mipAfterReload = 0;
    m_selectingMip = false;
    m_channelMode = 0;
    m_alphaUsageOverride = ImageCore::AlphaUsage::Auto;
    m_request.mipLevel = 0;

    // Fast reselect path via ImageGpuResourceCache.
    const ImageSourceIdentity identity = ImageSourceIdentity::ForPath(normalized);
    CachedPresentation cached {};
    if (TryGetCachedPresentation(normalized, 0, identity, cached))
    {
        m_filePath = normalized;
        m_binding.MarkSettled();
        m_request.source = normalized;
        m_request.mipLevel = 0;
        cached.replacesSource = true;
        PresentCached(std::move(cached));
        Invalidate();
        return S_OK;
    }

    // Back to the source that is still on screen, but its file changed since
    // it was decoded: it no longer counts as presented, so it is decoded again.
    if (m_presentationKind != PresentationKind::None &&
        m_loadedFilePath == normalized &&
        m_presentationIdentity != identity)
    {
        SetLoadedFilePath({});
        ResetPresentation();
        m_cpuPayload = {};
        m_needsCpuReupload = false;
    }

    m_filePath = normalized;
    m_binding.ClearFailure();
    m_forceCpuDecode.store(false);
    m_binding.MarkSettled();
    m_request.source = normalized;
    m_request.mipLevel = 0;

    // Keep previous image content while the next image loads (no Clear).
    SyncTextureDrawState();
    return S_OK;
}

void ImageBrowserMainImage::ClearSource()
{
    m_binding.Cancel();
    m_binding.ClearFailure();
    m_stagedCacheHit.reset();

    m_filePath.clear();
    SetLoadedFilePath({});
    m_mipAfterReload = 0;
    ResetPresentation();
    m_selectingMip = false;
    m_channelMode = 0;
    m_alphaUsageOverride = ImageCore::AlphaUsage::Auto;
    m_cpuPayload = {};
    m_needsCpuReupload = false;
    m_request.source.clear();
    m_request.mipLevel = 0;

    if (m_texture)
    {
        m_texture->Clear();
    }

    Invalidate();
}

void ImageBrowserMainImage::SetInteractionEnabled(bool enabled)
{
    if (m_interactionEnabled == enabled)
    {
        return;
    }

    m_interactionEnabled = enabled;
    if (!m_interactionEnabled)
    {
        m_panArmed = false;
        m_panning = false;
        m_pointerZoomActive = false;
        if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
        {
            if (GetCapture() == BackplateRef()->Window())
            {
                ReleaseCapture();
            }
        }
    }
}

void ImageBrowserMainImage::SetOnClick(ClickHandler handler)
{
    m_onClick = std::move(handler);
}

void ImageBrowserMainImage::SetLoadingSpinnerEnabled(bool enabled)
{
    if (m_loadingSpinnerEnabled == enabled)
    {
        return;
    }
    m_loadingSpinnerEnabled = enabled;
    Invalidate();
}

void ImageBrowserMainImage::SetAlphaCheckerboardEnabled(bool enabled)
{
    if (m_alphaCheckerboardEnabled == enabled)
    {
        return;
    }
    m_alphaCheckerboardEnabled = enabled;
    SyncTextureDrawState();
    Invalidate();
}

void ImageBrowserMainImage::SetHighQualitySampling(bool enabled)
{
    if (m_highQualitySampling == enabled)
    {
        return;
    }
    m_highQualitySampling = enabled;
    SyncTextureDrawState();
    Invalidate();
}

void ImageBrowserMainImage::ToggleSamplingQuality()
{
    m_highQualitySampling = !m_highQualitySampling;
    SyncTextureDrawState();
    Invalidate();
}

void ImageBrowserMainImage::RequestImageLoad()
{
    if (m_filePath.empty() || m_binding.IsLoading())
    {
        return;
    }

    if (m_binding.IsFailedFor(m_filePath))
    {
        return;
    }

    if (HasDisplayedContentForCurrentPath() || HasStagedContentForCurrentRequest())
    {
        return;
    }

    m_request.source = m_filePath;
    m_request.purpose = ImageCore::ImagePurpose::FullResolution;
    // m_request.mipLevel is owned by SetSourceFile / SelectMip.
    // What this decode is made from: validates the GPU cache entry it creates.
    m_requestIdentity = ImageSourceIdentity::ForPath(m_filePath);

    if (m_forceCpuDecode.load() || BackplateRef() == nullptr || BackplateRef()->D3DDevice() == nullptr)
    {
        m_request.allowGpuCompressedDDS = false;
    }
    else
    {
        m_request.allowGpuCompressedDDS = true;
    }

    m_binding.RequestLoad(m_request, m_filePath);
}

void ImageBrowserMainImage::TryReuploadCpuPayload(ID2D1RenderTarget* target)
{
    if (!m_needsCpuReupload || !target || !m_texture)
    {
        return;
    }

    // The payload belongs to the presentation on screen, which may still be the
    // previous source while the requested one loads.
    if (!m_cpuPayload.blocks || m_cpuPayload.sourcePath.empty() || m_cpuPayload.sourcePath != m_loadedFilePath)
    {
        m_needsCpuReupload = false;
        return;
    }

    const ImageAlphaInfo alpha = AlphaInfoFromPayload(m_cpuPayload);

    // Recreate the same kind of presentation where possible: a BGRA payload goes
    // back to an SRV on the (new) D3D device, so GPU/channel presentation is kept.
    if (!m_forceCpuDecode.load())
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(CreateBgraShaderResource(m_cpuPayload, srv)))
        {
            m_texture->SetShaderResource(srv);
            ApplyPresentation(PresentationKind::ShaderResource, m_cpuPayload.sourcePath, m_cpuPayload.width,
                m_cpuPayload.height, m_cpuPayload.format, alpha, m_cpuPayload.sourceMipLevels,
                m_cpuPayload.sourceMipIndex, m_presentationIdentity);
            ImageGpuResourceCache::Instance().Put(m_cpuPayload.sourcePath, m_cpuPayload.sourceMipIndex, srv,
                m_cpuPayload.width, m_cpuPayload.height, DXGI_FORMAT_B8G8R8A8_UNORM, m_alpha,
                m_sourceMipLevels, m_sourceMipIndex, m_presentationIdentity, DeviceGeneration());
            return;
        }
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    const HRESULT hrBmp = CreatePremultipliedD2DBitmap(
        target, m_cpuPayload, m_alphaUsageOverride, d2dBitmap);
    if (SUCCEEDED(hrBmp) && d2dBitmap)
    {
        m_texture->SetBitmap(d2dBitmap);
        ApplyPresentation(PresentationKind::Bitmap, m_cpuPayload.sourcePath, m_cpuPayload.width,
            m_cpuPayload.height, m_cpuPayload.format, alpha, m_cpuPayload.sourceMipLevels,
            m_cpuPayload.sourceMipIndex, m_presentationIdentity);
    }
    else
    {
        LogImageHr(L"D2D CreateBitmap (reupload)", m_cpuPayload.sourcePath, hrBmp);
        m_needsCpuReupload = false;
        SetLoadedFilePath({});
    }
}

void ImageBrowserMainImage::ApplyPendingPayload(ID2D1RenderTarget* target)
{
    const ImageAsyncBinding::Payload pending = m_binding.TakePending();
    if (!pending.blocks || pending.blocks->empty())
    {
        return;
    }

    // Path gate: only apply if pending still matches the current requested source.
    if (pending.sourcePath.empty() || pending.sourcePath != m_filePath)
    {
        return;
    }

    bool usedGpu = false;
    bool applied = false;

    if (IsCompressedDxgiFormat(pending.format) && BackplateRef() != nullptr)
    {
        ID3D11Device* dev = BackplateRef()->D3DDevice();
        if (dev && !m_forceCpuDecode.load())
        {
            D3D11_TEXTURE2D_DESC td {};
            td.Width = pending.width;
            td.Height = pending.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = pending.format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA init {};
            init.pSysMem = pending.blocks->data();
            init.SysMemPitch = pending.rowPitch;
            init.SysMemSlicePitch = static_cast<UINT>(pending.blocks->size());

            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            const auto t_tex = std::chrono::steady_clock::now();
            const HRESULT hrTex = dev->CreateTexture2D(&td, &init, &tex);
            const auto texMs = FIC2_ELAPSED_MS(t_tex);
            if (texMs > 30)
            {
                FIC2_LOG_INFO("[D3D] CreateTexture2D {}x{} fmt={} took {}ms",
                    pending.width, pending.height, static_cast<int>(pending.format), texMs);
            }

            if (SUCCEEDED(hrTex) && tex)
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                const HRESULT hrSrv = dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
                if (SUCCEEDED(hrSrv) && srv)
                {
                    if (m_texture)
                    {
                        m_texture->SetShaderResource(srv);
                    }
                    ApplyPresentation(PresentationKind::ShaderResource, pending.sourcePath, pending.width,
                        pending.height, pending.format, AlphaInfoFromPayload(pending), pending.sourceMipLevels,
                        pending.sourceMipIndex, m_requestIdentity);
                    // GPU-only content: nothing to re-upload from after a device change.
                    m_cpuPayload = {};

                    ImageGpuResourceCache::Instance().Put(
                        pending.sourcePath,
                        pending.sourceMipIndex,
                        srv,
                        pending.width,
                        pending.height,
                        pending.format,
                        m_alpha,
                        m_sourceMipLevels,
                        m_sourceMipIndex,
                        m_requestIdentity,
                        DeviceGeneration());

                    usedGpu = true;
                    applied = true;
                }
                else
                {
                    LogImageHr(L"D3D CreateShaderResourceView", pending.sourcePath, hrSrv);
                }
            }
            else
            {
                LogImageHr(L"D3D CreateTexture2D", pending.sourcePath, hrTex);
            }

            if (!applied)
            {
                m_forceCpuDecode.store(true);
                m_binding.MarkSettled();
                RequestImageLoad();
                usedGpu = true;
            }
        }
        else
        {
            m_forceCpuDecode.store(true);
            m_binding.MarkSettled();
            RequestImageLoad();
            usedGpu = true;
        }
    }

    // Prefer D3D SRV for uncompressed BGRA8 when available so channel isolation works.
    if (!usedGpu &&
        ImageAsyncBinding::IsCpuBgra8Format(pending.format) &&
        !m_forceCpuDecode.load())
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(CreateBgraShaderResource(pending, srv)))
        {
            if (m_texture)
            {
                m_texture->SetShaderResource(srv);
            }
            ApplyPresentation(PresentationKind::ShaderResource, pending.sourcePath, pending.width,
                pending.height, pending.format, AlphaInfoFromPayload(pending), pending.sourceMipLevels,
                pending.sourceMipIndex, m_requestIdentity);
            // Kept so a device change can rebuild the SRV without decoding again.
            m_cpuPayload = pending;

            ImageGpuResourceCache::Instance().Put(
                pending.sourcePath,
                pending.sourceMipIndex,
                srv,
                pending.width,
                pending.height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                m_alpha,
                m_sourceMipLevels,
                m_sourceMipIndex,
                m_requestIdentity,
                DeviceGeneration());

            usedGpu = true;
            applied = true;
        }
    }

    if (!usedGpu)
    {
        if (!target)
        {
            return;
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
        const HRESULT hrBmp = CreatePremultipliedD2DBitmap(
            target, pending, m_alphaUsageOverride, d2dBitmap);
        if (SUCCEEDED(hrBmp) && d2dBitmap)
        {
            if (m_texture)
            {
                m_texture->SetBitmap(d2dBitmap);
            }
            ApplyPresentation(PresentationKind::Bitmap, pending.sourcePath, pending.width,
                pending.height, pending.format, AlphaInfoFromPayload(pending), pending.sourceMipLevels,
                pending.sourceMipIndex, m_requestIdentity);
            m_cpuPayload = pending;
            applied = true;
        }
        else
        {
            LogImageHr(L"D2D CreateBitmap", pending.sourcePath, hrBmp);
            m_binding.RecordFailure(pending.sourcePath, hrBmp);
            m_binding.MarkSettled();
        }
    }

    if (applied)
    {
        m_binding.MarkSettled();

        // A replaced source was reloaded at mip 0 on behalf of a mip selection:
        // now that its size and mip count are known, continue to that mip.
        if (m_mipAfterReload != 0 && m_sourceMipIndex == 0)
        {
            const uint32_t mip = m_mipAfterReload;
            m_mipAfterReload = 0;
            if (mip < MipLevels())
            {
                (void)SelectMip(mip);
            }
        }
    }
}

bool ImageBrowserMainImage::TryGetCachedPresentation(
    const std::wstring& path,
    uint32_t mipLevel,
    const ImageSourceIdentity& identity,
    CachedPresentation& out) const
{
    if (BackplateRef() == nullptr)
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    const uint64_t deviceGeneration = DeviceGeneration();
    if (!ImageGpuResourceCache::Instance().TryGet(
            path,
            mipLevel,
            out.srv,
            width,
            height,
            out.format,
            out.alpha,
            out.sourceMipLevels,
            out.sourceMipIndex,
            identity,
            deviceGeneration))
    {
        return false;
    }

    out.path = path;
    out.width = width;
    out.height = height;
    out.identity = identity;
    out.deviceGeneration = deviceGeneration;
    return true;
}

bool ImageBrowserMainImage::IsBetweenRenderPasses() const
{
    return m_presentedInD3DPass && BackplateRef() != nullptr && BackplateRef()->IsRendering();
}

bool ImageBrowserMainImage::HasStagedContentForCurrentRequest() const
{
    return m_stagedCacheHit &&
        !m_filePath.empty() &&
        m_stagedCacheHit->path == m_filePath &&
        m_stagedCacheHit->sourceMipIndex == m_request.mipLevel &&
        m_stagedCacheHit->deviceGeneration == DeviceGeneration();
}

void ImageBrowserMainImage::PresentCached(CachedPresentation&& cached)
{
    // This frame's D3D pass has drawn the current content (an SRV) or nothing
    // (a D2D bitmap, drawn by the D2D pass still to come). Installing the hit
    // now would leave the frame with the old SRV under a replacement bitmap,
    // or with neither image: it waits for the next frame's handoff instead.
    if (IsBetweenRenderPasses())
    {
        m_stagedCacheHit = std::move(cached);
        return;
    }

    m_stagedCacheHit.reset();
    InstallCached(cached);
}

void ImageBrowserMainImage::InstallCached(const CachedPresentation& cached)
{
    if (m_texture)
    {
        m_texture->SetShaderResource(cached.srv);
    }
    ApplyPresentation(PresentationKind::ShaderResource, cached.path, cached.width, cached.height, cached.format,
        cached.alpha, cached.sourceMipLevels, cached.sourceMipIndex, cached.identity);
    if (cached.replacesSource)
    {
        // GPU-only content: nothing of the previous source to re-upload.
        m_cpuPayload = {};
    }
    else
    {
        ClampPanToVisible();
        SyncTextureDrawState();
    }
}

void ImageBrowserMainImage::PresentPendingContent(ID2D1RenderTarget* target)
{
    // A bitmap replacement needs the target; without one nothing is taken, and
    // the next frame retries.
    if (target == nullptr)
    {
        return;
    }

    if (m_stagedCacheHit)
    {
        const CachedPresentation staged = std::move(*m_stagedCacheHit);
        m_stagedCacheHit.reset();
        if (staged.path == m_filePath &&
            staged.sourceMipIndex == m_request.mipLevel &&
            staged.deviceGeneration == DeviceGeneration())
        {
            InstallCached(staged);
        }
    }

    ApplyPendingPayload(target);
    TryReuploadCpuPayload(target);
}

void ImageBrowserMainImage::OnGraphicsInvalidated(
    FD2D::GraphicsInvalidationReason reason,
    const FD2D::GraphicsGeneration& generation)
{
    if (reason == FD2D::GraphicsInvalidationReason::Shutdown)
    {
        // Release every cached SRV so none keeps the device alive.
        ImageGpuResourceCache::Instance().Clear();
        Wnd::OnGraphicsInvalidated(reason, generation);
        return;
    }

    // Cached SRVs belong to the old device: release them now rather than on the
    // next cache call (the cache also clears itself on a generation change).
    if (generation.device != m_lastSeenDeviceGeneration)
    {
        m_lastSeenDeviceGeneration = generation.device;
        ImageGpuResourceCache::Instance().Clear();
    }

    // The generations of the presentation's own domain decide; FD2D::Image has
    // already dropped content from a changed domain.
    bool domainChanged = false;
    if (m_presentationKind == PresentationKind::ShaderResource)
    {
        domainChanged = !(m_presentationStamp == CurrentDeviceStamp());
    }
    else if (m_presentationKind == PresentationKind::Bitmap)
    {
        domainChanged = !(m_presentationStamp == CurrentTargetStamp());
    }

    const bool hasCpuPayload = m_cpuPayload.blocks && !m_cpuPayload.sourcePath.empty();
    switch (DecidePresentationRecovery(m_presentationKind, domainChanged, hasCpuPayload))
    {
    case PresentationRecovery::Keep:
        // Includes an SRV across a target-only recreation: no downgrade to D2D.
        break;

    case PresentationRecovery::Reupload:
        m_needsCpuReupload = true;
        Invalidate();
        break;

    case PresentationRecovery::Reload:
    {
        // GPU-only content was lost; decode the same source again at the
        // requested mip. The view transform is kept, and so is the full-source
        // metadata (size, mip count), which only a mip-0 decode would supply -
        // unless the lost content was a previous source still shown while the
        // requested one loads.
        const bool sameSource = (m_loadedFilePath == m_filePath);
        const uint32_t sourceWidth = m_sourceWidth;
        const uint32_t sourceHeight = m_sourceHeight;
        const uint32_t sourceMipLevels = m_sourceMipLevels;
        SetLoadedFilePath({});
        ResetPresentation();
        if (sameSource)
        {
            m_sourceWidth = sourceWidth;
            m_sourceHeight = sourceHeight;
            m_sourceMipLevels = sourceMipLevels;
        }
        Invalidate();
        break;
    }
    }

    Wnd::OnGraphicsInvalidated(reason, generation);
}

void ImageBrowserMainImage::OnRender(ID2D1RenderTarget* target)
{
    // The handoff of a frame that has a D3D pass ran there; content that
    // arrived since then waits for the next frame's handoff.
    const bool presentedInD3DPass = m_presentedInD3DPass;
    m_presentedInD3DPass = false;

    if (target == nullptr)
    {
        return;
    }

    AdvanceZoomAnimation(CommonUtil::NowMs());
    if (!presentedInD3DPass)
    {
        // D2D-only frame: this pass draws everything, so the handoff is here.
        PresentPendingContent(target);
    }

    if (!m_binding.IsLoading() && !m_filePath.empty())
    {
        if (m_binding.IsFailedFor(m_filePath))
        {
            if (m_selectingMip)
            {
                // Keep the previously displayed mip and allow future attempts.
                m_selectingMip = false;
                m_request.mipLevel = m_sourceMipIndex;
                m_binding.ClearFailure();
                m_binding.MarkSettled();
            }
            else if (m_spinner)
            {
                m_spinner->SetActive(false);
            }
            SyncTextureDrawState();
            Wnd::OnRender(target);
            return;
        }

        if (!HasDisplayedContentForCurrentPath())
        {
            RequestImageLoad();
        }
    }

    SyncTextureDrawState();

    const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_binding.IsLoading();
    if (m_spinner)
    {
        m_spinner->SetActive(shouldShowSpinner);
    }

    Wnd::OnRender(target);
}

void ImageBrowserMainImage::OnRenderD3D(ID3D11DeviceContext* context)
{
    AdvanceZoomAnimation(CommonUtil::NowMs());

    // Frame handoff. The D3D pass draws before the D2D pass, so a replacement
    // (decoded payload, staged cache hit or CPU re-upload) is installed here,
    // before either pass draws this control: an SRV is drawn by this pass and a
    // D2D bitmap by the D2D pass, and the previous content - which the
    // replacement removed from the texture - is drawn by neither. Without a
    // replacement the previous content stays and both passes draw it.
    PresentPendingContent(BackplateRef() != nullptr ? BackplateRef()->RenderTarget() : nullptr);
    m_presentedInD3DPass = true;

    SyncTextureDrawState();
    Wnd::OnRenderD3D(context);
}

void ImageBrowserMainImage::SetZoomScale(float scale)
{
    constexpr float kMinZoom = 0.1f;
    constexpr float kMaxZoom = 50.0f;
    m_targetZoomScale = std::max(kMinZoom, std::min(kMaxZoom, scale));
    m_lastZoomAnimMs = CommonUtil::NowMs();
    if (BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }
    Invalidate();
}

void ImageBrowserMainImage::ResetZoom()
{
    m_targetZoomScale = 1.0f;
    m_zoomVelocity = 0.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
    m_panning = false;
    m_pointerZoomActive = false;
    m_lastZoomAnimMs = CommonUtil::NowMs();
    if (BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }
    SyncTextureDrawState();
    Invalidate();
}

void ImageBrowserMainImage::SetZoomStiffness(float stiffness)
{
    m_zoomStiffness = std::max(10.0f, std::min(500.0f, stiffness));
}

void ImageBrowserMainImage::AdvanceZoomAnimation(unsigned long long nowMs)
{
    const float zoomBefore = m_zoomScale;
    const float panXBefore = m_panX;
    const float panYBefore = m_panY;

    if (m_lastZoomAnimMs == 0)
    {
        m_lastZoomAnimMs = nowMs;
    }

    const unsigned long long elapsed = nowMs - m_lastZoomAnimMs;
    m_lastZoomAnimMs = nowMs;

    if (elapsed == 0)
    {
        return;
    }

    const float dt = static_cast<float>(elapsed) / 1000.0f;
    const float stiffness = m_zoomStiffness;
    const float damping = 2.0f * std::sqrt(stiffness);
    const float diff = m_targetZoomScale - m_zoomScale;

    m_zoomVelocity += (diff * stiffness - m_zoomVelocity * damping) * dt;
    m_zoomScale += m_zoomVelocity * dt;

    if (m_pointerZoomActive && !m_panning)
    {
        const float startZoom = (m_pointerZoomStartZoom > 0.0001f) ? m_pointerZoomStartZoom : 0.0001f;
        const float ratio = m_zoomScale / startZoom;

        const D2D1_RECT_F r = LayoutRect();
        const float centerX = (r.left + r.right) * 0.5f;
        const float centerY = (r.top + r.bottom) * 0.5f;
        const float dx = m_pointerZoomMouseX - centerX;
        const float dy = m_pointerZoomMouseY - centerY;

        m_panX = dx - ((dx - m_pointerZoomStartPanX) * ratio);
        m_panY = dy - ((dy - m_pointerZoomStartPanY) * ratio);
    }

    ClampPanToVisible();

    if (std::abs(diff) < 0.001f && std::abs(m_zoomVelocity) < 0.001f)
    {
        m_zoomScale = m_targetZoomScale;
        m_zoomVelocity = 0.0f;
        m_pointerZoomActive = false;
    }
    else if (BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }

    // Synchronized panes follow only real changes of the effective transform;
    // a settled animation called from every render pass must not keep
    // re-broadcasting (and invalidating the other pane).
    const bool changed = m_zoomScale != zoomBefore || m_panX != panXBefore || m_panY != panYBefore;
    if (changed && !m_suppressViewNotify && m_onViewChanged)
    {
        m_onViewChanged(GetViewTransform());
    }
}

bool ImageBrowserMainImage::OnInputEvent(const FD2D::InputEvent& event)
{
    if (!m_interactionEnabled)
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
        case FD2D::InputEventType::MouseMove:
        case FD2D::InputEventType::MouseUp:
        case FD2D::InputEventType::MouseWheel:
            return false;
        default:
            break;
        }
    }

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
            m_panArmed = true;
            m_panning = false;
            m_pointerZoomActive = false;
            m_panStartX = static_cast<float>(pt.x);
            m_panStartY = static_cast<float>(pt.y);
            m_panStartOffsetX = m_panX;
            m_panStartOffsetY = m_panY;

            if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
            {
                SetCapture(BackplateRef()->Window());
            }
            return true;
        }
        break;
    }
    case FD2D::InputEventType::MouseMove:
    {
        if (!event.hasPoint)
        {
            break;
        }
        if (m_panArmed || m_panning)
        {
            POINT pt = event.point;
            const float screenDeltaX = static_cast<float>(pt.x) - m_panStartX;
            const float screenDeltaY = static_cast<float>(pt.y) - m_panStartY;

            if (!m_panning)
            {
                constexpr float kStartThresholdPx = 3.0f;
                if (std::abs(screenDeltaX) >= kStartThresholdPx ||
                    std::abs(screenDeltaY) >= kStartThresholdPx)
                {
                    m_panning = true;
                }
            }

            if (m_panning)
            {
                float deltaX = screenDeltaX;
                float deltaY = screenDeltaY;
                if (m_rotationQuarters != 0)
                {
                    const int inverseQuarters = (4 - m_rotationQuarters) % 4;
                    RotateVectorByQuarters(screenDeltaX, screenDeltaY, inverseQuarters, deltaX, deltaY);
                }

                m_panX = m_panStartOffsetX + deltaX;
                m_panY = m_panStartOffsetY + deltaY;
                ClampPanToVisible();
                m_pointerZoomActive = false;
                SyncTextureDrawState();
                Invalidate();
                if (!m_suppressViewNotify && m_onViewChanged)
                {
                    m_onViewChanged(GetViewTransform());
                }
                return true;
            }

            return true;
        }
        break;
    }
    case FD2D::InputEventType::MouseUp:
    {
        if (event.button != FD2D::MouseButton::Left)
        {
            break;
        }
        if (m_panArmed || m_panning)
        {
            const bool wasPanning = m_panning;
            m_panning = false;
            m_panArmed = false;

            if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
            {
                ReleaseCapture();
            }

            if (!wasPanning && m_onClick)
            {
                m_onClick();
            }
            return true;
        }
        break;
    }
    case FD2D::InputEventType::CaptureChanged:
    {
        if ((m_panArmed || m_panning) &&
            BackplateRef() != nullptr &&
            GetCapture() != BackplateRef()->Window())
        {
            m_panning = false;
            m_panArmed = false;
        }
        break;
    }
    case FD2D::InputEventType::MouseWheel:
    {
        if (!event.hasPoint)
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
            const short delta = event.wheelDelta;
            const bool shiftPressed = event.modifiers.shift;
            const float zoomStep = shiftPressed ? 0.1f : 0.5f;
            const float zoomFactor = (delta > 0) ? (1.0f + zoomStep) : (1.0f / (1.0f + zoomStep));
            const float newZoom = m_targetZoomScale * zoomFactor;

            m_pointerZoomActive = true;
            m_pointerZoomStartZoom = m_zoomScale;
            m_pointerZoomStartPanX = m_panX;
            m_pointerZoomStartPanY = m_panY;
            m_pointerZoomMouseX = static_cast<float>(pt.x);
            m_pointerZoomMouseY = static_cast<float>(pt.y);

            SetZoomScale(newZoom);

            if (!m_suppressViewNotify && m_onViewChanged)
            {
                m_onViewChanged(GetViewTransform());
            }
            return true;
        }
        break;
    }
    default:
        break;
    }

    return Wnd::OnInputEvent(event);
}
