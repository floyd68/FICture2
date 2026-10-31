#include "ImageBrowserMainImagePresentation.h"

#include <algorithm>

void MainImagePresentation::Apply(
    PresentationKind newKind,
    uint32_t newWidth,
    uint32_t newHeight,
    DXGI_FORMAT newFormat,
    const ImageAlphaInfo& newAlpha,
    uint32_t newMipLevels,
    uint32_t newMipIndex,
    const ImageSourceIdentity& newIdentity,
    const FD2D::GraphicsResourceStamp& deviceStamp,
    const FD2D::GraphicsResourceStamp& targetStamp)
{
    width = newWidth;
    height = newHeight;
    format = newFormat;
    alpha = newAlpha;
    mipLevels = (std::max)(1u, newMipLevels);
    mipIndex = newMipIndex;
    if (newMipIndex == 0)
    {
        sourceWidth = newWidth;
        sourceHeight = newHeight;
    }
    kind = newKind;
    // An SRV belongs to the device, a D2D bitmap to the render target.
    stamp = (newKind == PresentationKind::ShaderResource) ? deviceStamp : targetStamp;
    identity = newIdentity;
}

void MainImagePresentation::ResetKeepingSource()
{
    const uint32_t keptWidth = sourceWidth;
    const uint32_t keptHeight = sourceHeight;
    const uint32_t keptMipLevels = mipLevels;
    Reset();
    sourceWidth = keptWidth;
    sourceHeight = keptHeight;
    mipLevels = keptMipLevels;
}

bool MainImagePresentation::DomainChanged(
    const FD2D::GraphicsResourceStamp& deviceStamp,
    const FD2D::GraphicsResourceStamp& targetStamp) const
{
    if (kind == PresentationKind::ShaderResource)
    {
        return !(stamp == deviceStamp);
    }
    if (kind == PresentationKind::Bitmap)
    {
        return !(stamp == targetStamp);
    }
    return false;
}
