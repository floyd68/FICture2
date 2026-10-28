#include "ShellThumbnailSource.h"

#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{
    class SystemShellThumbnailSource final : public IShellThumbnailSource
    {
    public:
        ShellThumbnailResult GetThumbnail(const std::wstring& path, uint32_t width, uint32_t height) override
        {
            ShellThumbnailResult result {};
            Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
            HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
            if (FAILED(hr) || !factory)
            {
                result.hr = FAILED(hr) ? hr : E_NOINTERFACE;
                return result;
            }

            const SIZE size { static_cast<LONG>(width), static_cast<LONG>(height) };
            HBITMAP bitmap = nullptr;
            hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY, &bitmap);
            if (hr == WTS_E_EXTRACTIONPENDING)
            {
                result.status = ShellThumbnailStatus::Pending;
                result.hr = hr;
                return result;
            }
            if (FAILED(hr) || bitmap == nullptr)
            {
                result.hr = FAILED(hr) ? hr : E_FAIL;
                return result;
            }

            hr = ShellBitmapToDecodedImage(bitmap, result.image);
            DeleteObject(bitmap);
            result.hr = hr;
            result.status = SUCCEEDED(hr) ? ShellThumbnailStatus::Ready : ShellThumbnailStatus::Failed;
            return result;
        }
    };
}

IShellThumbnailSource& IShellThumbnailSource::System()
{
    static SystemShellThumbnailSource s_source;
    return s_source;
}

HRESULT ShellBitmapToDecodedImage(HBITMAP bitmap, ImageCore::DecodedImage& out)
{
    BITMAP info {};
    if (bitmap == nullptr || GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) ||
        info.bmWidth <= 0 || info.bmHeight == 0)
    {
        return E_INVALIDARG;
    }
    const uint32_t width = static_cast<uint32_t>(info.bmWidth);
    const uint32_t height = static_cast<uint32_t>(std::abs(info.bmHeight));
    const uint32_t rowPitch = width * 4u;
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(rowPitch) * height);

    // GetDIBits converts any bitmap to 32 bpp in the requested (top-down) row
    // order; a 32 bpp source keeps its alpha bytes, others read as 0.
    BITMAPINFO header {};
    header.bmiHeader.biSize = sizeof(header.bmiHeader);
    header.bmiHeader.biWidth = static_cast<LONG>(width);
    header.bmiHeader.biHeight = -static_cast<LONG>(height);
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const int lines = GetDIBits(dc, bitmap, 0, height, pixels->data(), &header, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines != static_cast<int>(height))
    {
        return E_FAIL;
    }

    bool anyAlpha = false;
    bool allOpaque = true;
    for (size_t i = 3; i < pixels->size(); i += 4)
    {
        const uint8_t alpha = (*pixels)[i];
        anyAlpha = anyAlpha || alpha != 0;
        allOpaque = allOpaque && alpha == 0xFF;
    }

    ImageCore::AlphaEncoding encoding = ImageCore::AlphaEncoding::Opaque;
    if (!anyAlpha)
    {
        for (size_t i = 3; i < pixels->size(); i += 4)
        {
            (*pixels)[i] = 0xFF;
        }
    }
    else if (!allOpaque)
    {
        // GDI alpha is premultiplied; ImageCore payloads carry straight alpha.
        for (size_t i = 0; i < pixels->size(); i += 4)
        {
            const uint32_t alpha = (*pixels)[i + 3];
            for (size_t c = 0; c < 3; ++c)
            {
                uint8_t& value = (*pixels)[i + c];
                value = alpha == 0 ? 0 : static_cast<uint8_t>((std::min)(255u, (value * 255u + alpha / 2u) / alpha));
            }
        }
        encoding = ImageCore::AlphaEncoding::Straight;
    }

    out = {};
    out.width = width;
    out.height = height;
    out.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    out.alphaEncoding = encoding;
    out.rowPitchBytes = rowPitch;
    out.blockBytes = static_cast<uint32_t>(pixels->size());
    out.blocks = std::move(pixels);
    out.sourceMipLevels = 1;
    out.sourceMipIndex = 0;
    return S_OK;
}
