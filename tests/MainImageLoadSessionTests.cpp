#include "TestHarness.h"

#include "ImageBrowserMainImageLoadSession.h"

// What the main image is loading: the requested source and mip, the request
// built for a decode, a mip selection ending by presentation or failure, the
// mip deferred until a reload shows mip 0, and the CPU-decode fallback.

namespace
{
    ImageSourceIdentity Identity(uint64_t size)
    {
        ImageSourceIdentity identity {};
        identity.size = size;
        identity.writeTime = 1000 + size;
        identity.known = true;
        return identity;
    }
}

FIC2_TEST(MainImageLoadSession_SetReplaceAndClearSource)
{
    MainImageLoadSession s;
    FIC2_CHECK(s.Path().empty());
    FIC2_CHECK(s.RequestedMip() == 0);
    FIC2_CHECK(!s.SelectingMip());
    FIC2_CHECK(!s.CpuDecodeForced());

    s.SetSource(L"c:\\synthetic\\a.dds");
    FIC2_CHECK(s.Path() == L"c:\\synthetic\\a.dds");
    FIC2_CHECK(s.PrepareRequest(Identity(1), true).source == L"c:\\synthetic\\a.dds");

    // A new source asks for mip 0 again; the selection state is left to ResetMip.
    s.BeginMipSelection(3);
    s.SetSource(L"c:\\synthetic\\b.dds");
    FIC2_CHECK(s.Path() == L"c:\\synthetic\\b.dds");
    FIC2_CHECK(s.RequestedMip() == 0);
    FIC2_CHECK(s.SelectingMip());
    s.ResetMip();
    FIC2_CHECK(!s.SelectingMip());

    s.BeginMipSelection(2);
    s.SetMipAfterReload(4);
    s.Clear();
    FIC2_CHECK(s.Path().empty());
    FIC2_CHECK(s.RequestedMip() == 0);
    FIC2_CHECK(!s.SelectingMip());
    FIC2_CHECK(s.MipAfterReload() == 0);
    FIC2_CHECK(s.PrepareRequest(ImageSourceIdentity {}, true).source.empty());
}

FIC2_TEST(MainImageLoadSession_PrepareRequest)
{
    MainImageLoadSession s;
    s.SetSource(L"c:\\synthetic\\a.dds");
    s.BeginMipSelection(2);

    const ImageCore::ImageRequest& gpu = s.PrepareRequest(Identity(1), true);
    FIC2_CHECK(gpu.source == L"c:\\synthetic\\a.dds");
    FIC2_CHECK(gpu.purpose == ImageCore::ImagePurpose::FullResolution);
    FIC2_CHECK(gpu.mipLevel == 2);
    FIC2_CHECK(gpu.allowGpuCompressedDDS);
    FIC2_CHECK(s.RequestIdentity() == Identity(1));

    // No D3D device: CPU pixels.
    FIC2_CHECK(!s.PrepareRequest(Identity(2), false).allowGpuCompressedDDS);
    FIC2_CHECK(s.RequestIdentity() == Identity(2));

    // A failed GPU upload forces CPU decoding even with a device.
    s.ForceCpuDecode();
    FIC2_CHECK(s.CpuDecodeForced());
    FIC2_CHECK(!s.PrepareRequest(Identity(2), true).allowGpuCompressedDDS);
    s.ResetCpuDecode();
    FIC2_CHECK(s.PrepareRequest(Identity(2), true).allowGpuCompressedDDS);
}

FIC2_TEST(MainImageLoadSession_MipSelectionEnds)
{
    MainImageLoadSession s;
    s.SetSource(L"c:\\synthetic\\a.dds");

    // Ended by presentation: the requested mip stays.
    s.BeginMipSelection(3);
    FIC2_CHECK(s.SelectingMip());
    FIC2_CHECK(s.RequestedMip() == 3);
    s.EndMipSelection();
    FIC2_CHECK(!s.SelectingMip());
    FIC2_CHECK(s.RequestedMip() == 3);
    FIC2_CHECK(!s.FailMipSelection(1));   // not a selection any more
    FIC2_CHECK(s.RequestedMip() == 3);

    // Ended by failure: back to the shown mip.
    s.BeginMipSelection(5);
    FIC2_CHECK(s.FailMipSelection(3));
    FIC2_CHECK(!s.SelectingMip());
    FIC2_CHECK(s.RequestedMip() == 3);

    // A selection re-enables the GPU path.
    s.ForceCpuDecode();
    s.BeginMipSelection(1);
    FIC2_CHECK(!s.CpuDecodeForced());
}

FIC2_TEST(MainImageLoadSession_MipAfterReload)
{
    MainImageLoadSession s;
    s.SetSource(L"c:\\synthetic\\a.dds");
    FIC2_CHECK(s.TakeMipAfterReload(0) == 0);   // nothing pending

    s.SetMipAfterReload(4);
    // Reloading keeps the pending mip (it is recorded before the reload).
    s.BeginReload();
    FIC2_CHECK(s.MipAfterReload() == 4);
    // Taken only once mip 0 is shown, then dropped.
    FIC2_CHECK(s.TakeMipAfterReload(2) == 0);
    FIC2_CHECK(s.MipAfterReload() == 4);
    FIC2_CHECK(s.TakeMipAfterReload(0) == 4);
    FIC2_CHECK(s.MipAfterReload() == 0);
    FIC2_CHECK(s.TakeMipAfterReload(0) == 0);

    // A new source (ResetMip) or a clear drops it.
    s.SetMipAfterReload(3);
    s.ResetMip();
    FIC2_CHECK(s.TakeMipAfterReload(0) == 0);
    s.SetMipAfterReload(3);
    s.Clear();
    FIC2_CHECK(s.TakeMipAfterReload(0) == 0);
}

FIC2_TEST(MainImageLoadSession_ReloadResets)
{
    MainImageLoadSession s;
    s.SetSource(L"c:\\synthetic\\a.dds");
    s.BeginMipSelection(3);
    s.ForceCpuDecode();

    s.BeginReload();
    FIC2_CHECK(s.Path() == L"c:\\synthetic\\a.dds");
    FIC2_CHECK(s.RequestedMip() == 0);
    FIC2_CHECK(!s.SelectingMip());
    FIC2_CHECK(!s.CpuDecodeForced());
}
