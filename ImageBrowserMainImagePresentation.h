#pragma once

#include "FD2D/Wnd.h"
#include "ImageAlphaPresentation.h"
#include "ImagePresentationRecovery.h"
#include "ImageSourceIdentity.h"

#include <dxgiformat.h>

#include <cstdint>

// What the main image shows, without UI objects: the kind of resource that
// holds it, the graphics domain that resource belongs to, the source identity
// it was decoded from, and the loaded metadata (size, format, alpha, mip).
// The presented path and the resources themselves stay with the control.
struct MainImagePresentation
{
    PresentationKind kind { PresentationKind::None };
    // The device's stamp for an SRV, the render target's for a D2D bitmap.
    FD2D::GraphicsResourceStamp stamp {};
    ImageSourceIdentity identity {};

    uint32_t width { 0 };               // of the shown mip
    uint32_t height { 0 };
    DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
    ImageAlphaInfo alpha {};
    uint32_t mipLevels { 1 };           // of the source; at least 1
    uint32_t mipIndex { 0 };            // the shown mip
    uint32_t sourceWidth { 0 };         // of mip 0, as last seen
    uint32_t sourceHeight { 0 };

    bool Shown() const { return kind != PresentationKind::None; }
    // Shown from a D3D shader resource (else a D2D bitmap, or nothing).
    bool Gpu() const { return kind == PresentationKind::ShaderResource; }

    // Records new content. The source size changes only with mip 0 (another
    // mip keeps the full size seen before). `deviceStamp` / `targetStamp` are
    // the current ones; the kind decides which the content belongs to.
    void Apply(
        PresentationKind newKind,
        uint32_t newWidth,
        uint32_t newHeight,
        DXGI_FORMAT newFormat,
        const ImageAlphaInfo& newAlpha,
        uint32_t newMipLevels,
        uint32_t newMipIndex,
        const ImageSourceIdentity& newIdentity,
        const FD2D::GraphicsResourceStamp& deviceStamp,
        const FD2D::GraphicsResourceStamp& targetStamp);

    // Nothing is shown.
    void Reset() { *this = {}; }
    // Nothing is shown, but the source's size and mip count are kept (the
    // content was lost and the same source is decoded again).
    void ResetKeepingSource();

    // Shown content decoded from a different version of the source.
    bool StaleFor(const ImageSourceIdentity& current) const { return Shown() && identity != current; }
    // The graphics domain the shown content belongs to is gone (the device
    // for an SRV, the render target for a bitmap); false when nothing is shown.
    bool DomainChanged(const FD2D::GraphicsResourceStamp& deviceStamp, const FD2D::GraphicsResourceStamp& targetStamp) const;
};
