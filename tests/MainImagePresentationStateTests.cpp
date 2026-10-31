#include "TestHarness.h"

#include "ImageBrowserMainImagePresentation.h"

// What the main image shows: recording new content (mip 0 and later mips),
// resetting (also keeping the source metadata for a reload), the stale
// identity check and whether the content's graphics domain is gone.

namespace
{
    FD2D::GraphicsResourceStamp Stamp(uint64_t generation)
    {
        FD2D::GraphicsResourceStamp stamp {};
        stamp.generation = generation;
        stamp.valid = true;
        return stamp;
    }

    ImageSourceIdentity Identity(uint64_t size)
    {
        ImageSourceIdentity identity {};
        identity.size = size;
        identity.writeTime = 1000 + size;
        identity.known = true;
        return identity;
    }

    const FD2D::GraphicsResourceStamp kDevice = Stamp(7);
    const FD2D::GraphicsResourceStamp kTarget = Stamp(3);
}

FIC2_TEST(MainImagePresentationState_ApplyRecordsTheContent)
{
    MainImagePresentation p;
    FIC2_CHECK(!p.Shown());
    FIC2_CHECK(!p.Gpu());
    FIC2_CHECK(p.mipLevels == 1);

    ImageAlphaInfo alpha {};
    alpha.encoding = ImageCore::AlphaEncoding::Premultiplied;
    p.Apply(PresentationKind::ShaderResource, 1024, 512, DXGI_FORMAT_BC7_UNORM, alpha, 11, 0, Identity(1),
        kDevice, kTarget);
    FIC2_CHECK(p.Shown());
    FIC2_CHECK(p.Gpu());
    FIC2_CHECK(p.width == 1024 && p.height == 512);
    FIC2_CHECK(p.format == DXGI_FORMAT_BC7_UNORM);
    FIC2_CHECK(p.alpha.encoding == ImageCore::AlphaEncoding::Premultiplied);
    FIC2_CHECK(p.mipLevels == 11 && p.mipIndex == 0);
    FIC2_CHECK(p.sourceWidth == 1024 && p.sourceHeight == 512);   // mip 0 is the source size
    FIC2_CHECK(p.stamp == kDevice);                                // an SRV belongs to the device
    FIC2_CHECK(p.identity == Identity(1));

    // A later mip keeps the source size seen at mip 0.
    p.Apply(PresentationKind::ShaderResource, 256, 128, DXGI_FORMAT_BC7_UNORM, alpha, 11, 2, Identity(1),
        kDevice, kTarget);
    FIC2_CHECK(p.width == 256 && p.height == 128 && p.mipIndex == 2);
    FIC2_CHECK(p.sourceWidth == 1024 && p.sourceHeight == 512);

    // A D2D bitmap belongs to the render target; the mip count is at least 1.
    p.Apply(PresentationKind::Bitmap, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, {}, 0, 0, Identity(2), kDevice, kTarget);
    FIC2_CHECK(p.Shown());
    FIC2_CHECK(!p.Gpu());
    FIC2_CHECK(p.stamp == kTarget);
    FIC2_CHECK(p.mipLevels == 1);
    FIC2_CHECK(p.sourceWidth == 64 && p.sourceHeight == 64);
}

FIC2_TEST(MainImagePresentationState_ResetAndResetKeepingSource)
{
    MainImagePresentation p;
    p.Apply(PresentationKind::ShaderResource, 256, 128, DXGI_FORMAT_BC1_UNORM, {}, 9, 2, Identity(1), kDevice, kTarget);
    p.sourceWidth = 1024;
    p.sourceHeight = 512;

    MainImagePresentation kept = p;
    kept.ResetKeepingSource();
    FIC2_CHECK(!kept.Shown());
    FIC2_CHECK(kept.width == 0 && kept.height == 0 && kept.format == DXGI_FORMAT_UNKNOWN);
    FIC2_CHECK(kept.mipIndex == 0);
    FIC2_CHECK(!kept.identity.known);
    FIC2_CHECK(!kept.stamp.valid);
    // What only a mip-0 decode would supply again is kept.
    FIC2_CHECK(kept.sourceWidth == 1024 && kept.sourceHeight == 512);
    FIC2_CHECK(kept.mipLevels == 9);

    p.Reset();
    FIC2_CHECK(!p.Shown());
    FIC2_CHECK(p.width == 0 && p.sourceWidth == 0 && p.sourceHeight == 0);
    FIC2_CHECK(p.mipLevels == 1 && p.mipIndex == 0);
    FIC2_CHECK(!p.stamp.valid && !p.identity.known);
}

FIC2_TEST(MainImagePresentationState_StaleIdentity)
{
    MainImagePresentation p;
    // Nothing shown is never stale.
    FIC2_CHECK(!p.StaleFor(Identity(1)));

    p.Apply(PresentationKind::Bitmap, 8, 8, DXGI_FORMAT_B8G8R8A8_UNORM, {}, 1, 0, Identity(1), kDevice, kTarget);
    FIC2_CHECK(!p.StaleFor(Identity(1)));
    FIC2_CHECK(p.StaleFor(Identity(2)));        // the file was replaced
    FIC2_CHECK(p.StaleFor(ImageSourceIdentity {}));
}

FIC2_TEST(MainImagePresentationState_DomainChanged)
{
    MainImagePresentation p;
    // Nothing shown: nothing to lose.
    FIC2_CHECK(!p.DomainChanged(Stamp(99), Stamp(99)));

    // An SRV follows the device only.
    p.Apply(PresentationKind::ShaderResource, 8, 8, DXGI_FORMAT_BC1_UNORM, {}, 1, 0, Identity(1), kDevice, kTarget);
    FIC2_CHECK(!p.DomainChanged(kDevice, kTarget));
    FIC2_CHECK(!p.DomainChanged(kDevice, Stamp(4)));     // a target-only recreation keeps it
    FIC2_CHECK(p.DomainChanged(Stamp(8), kTarget));

    // A bitmap follows the render target only.
    p.Apply(PresentationKind::Bitmap, 8, 8, DXGI_FORMAT_B8G8R8A8_UNORM, {}, 1, 0, Identity(1), kDevice, kTarget);
    FIC2_CHECK(!p.DomainChanged(kDevice, kTarget));
    FIC2_CHECK(!p.DomainChanged(Stamp(8), kTarget));
    FIC2_CHECK(p.DomainChanged(kDevice, Stamp(4)));
}
