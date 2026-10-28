#pragma once

// Thumbnails of online-only cloud files from the Windows shell (#59).
//
// Explorer shows the thumbnail that the cloud sync provider supplies through
// the shell's thumbnail cache; reading the file's bytes instead would make the
// provider download (hydrate) the whole file. The system source asks
// IShellItemImageFactory::GetImage with SIIGBF_THUMBNAILONLY only:
// - a cached thumbnail is returned at once;
// - otherwise the shell asks the provider for one and returns
//   WTS_E_EXTRACTIONPENDING; the provider fetches the thumbnail (not the file)
//   in the background, and a later call returns it (Pending here);
// - no icon is substituted, and no call downloads the file. Verified on
//   OneDrive Files On-Demand JPEG/PNG placeholders: the files stayed online-only
//   (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) through cold and cached calls at
//   128 to 1024 pixels. SIIGBF_INCACHEONLY also never downloads, but it fails
//   for every thumbnail the cache does not hold yet, so it is not used.

#include "ImageCore/DecodedImage.h"

#include <windows.h>

#include <cstdint>
#include <string>

enum class ShellThumbnailStatus
{
    Ready,     // `image` holds the thumbnail
    Pending,   // the provider is fetching it: ask again later
    Failed,    // no thumbnail from the shell (`hr` says why)
};

struct ShellThumbnailResult
{
    ShellThumbnailStatus status { ShellThumbnailStatus::Failed };
    HRESULT hr { E_FAIL };
    ImageCore::DecodedImage image {};
};

class IShellThumbnailSource
{
public:
    virtual ~IShellThumbnailSource() = default;

    // The shell's thumbnail of `path`, fitted into `width` x `height` with its
    // aspect kept (it may be smaller). Runs on a CloudThumbnailRouter worker,
    // with COM initialized (STA); it must never download the file.
    virtual ShellThumbnailResult GetThumbnail(const std::wstring& path, uint32_t width, uint32_t height) = 0;

    // IShellItemImageFactory::GetImage with SIIGBF_THUMBNAILONLY.
    static IShellThumbnailSource& System();
};

// Copies a GDI bitmap (a DIB section or a device-dependent bitmap, either row
// order) into a top-down BGRA8 payload with straight alpha, as ImageCore
// decodes: a bitmap whose alpha is 0 everywhere (no alpha channel) or 255
// everywhere is opaque (alpha set to 255, AlphaEncoding::Opaque); otherwise the shell's premultiplied alpha is
// converted to straight (AlphaEncoding::Straight). Fails for an empty or
// invalid bitmap; does not take ownership of `bitmap`.
HRESULT ShellBitmapToDecodedImage(HBITMAP bitmap, ImageCore::DecodedImage& out);
