#include "TestHarness.h"

#include "ImageBrowserInfoPresenter.h"

#include <iterator>
#include <string>

// Format label and bit depth in the info bar for every DXGI format the main
// view can present, including the sRGB and signed block-compressed variants.

namespace
{
    std::wstring InfoFor(DXGI_FORMAT format)
    {
        ImageBrowserInfoPresenter::Input input {};
        input.activePath = L"C:\\images\\texture.dds";
        input.hasLoadedInfo = true;
        input.loadedInfo.width = 256;
        input.loadedInfo.height = 128;
        input.loadedInfo.format = format;
        return ImageBrowserInfoPresenter::Build(input).infoText;
    }

    bool StartsWith(const std::wstring& text, const std::wstring& prefix)
    {
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    struct Expected
    {
        DXGI_FORMAT format;
        const wchar_t* label;
        int bitsPerPixel;
    };

    void CheckLabels(const Expected* cases, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const Expected& c = cases[i];
            const std::wstring info = InfoFor(c.format);
            const std::wstring prefix =
                L"256 x 128 x " + std::to_wstring(c.bitsPerPixel) + L" | " + c.label;
            FIC2_CHECK(StartsWith(info, prefix));
            // The label is followed by a separator or the end of the text, so
            // "BC1" does not also accept "BC1 sRGB".
            FIC2_CHECK(info.size() == prefix.size() || info.compare(prefix.size(), 3, L" | ") == 0);
            FIC2_CHECK(info.find(L"Unknown") == std::wstring::npos);
            FIC2_CHECK(info.find(L"x ?") == std::wstring::npos);
        }
    }
}

FIC2_TEST(InfoPresenter_SrgbFormatsShowTheirLabelAndBitDepth)
{
    const Expected cases[] = {
        { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, L"R8G8B8A8 sRGB", 32 },
        { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, L"B8G8R8A8 sRGB", 32 },
        { DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, L"B8G8R8X8 sRGB", 32 },
        { DXGI_FORMAT_BC1_UNORM_SRGB, L"BC1 sRGB", 4 },
        { DXGI_FORMAT_BC2_UNORM_SRGB, L"BC2 sRGB", 8 },
        { DXGI_FORMAT_BC3_UNORM_SRGB, L"BC3 sRGB", 8 },
        { DXGI_FORMAT_BC7_UNORM_SRGB, L"BC7 sRGB", 8 },
    };
    CheckLabels(cases, std::size(cases));
}

FIC2_TEST(InfoPresenter_SignedBlockCompressedFormatsShowTheirLabelAndBitDepth)
{
    const Expected cases[] = {
        { DXGI_FORMAT_BC4_SNORM, L"BC4 SNORM", 4 },
        { DXGI_FORMAT_BC5_SNORM, L"BC5 SNORM", 8 },
        { DXGI_FORMAT_BC6H_SF16, L"BC6H SF16", 8 },
    };
    CheckLabels(cases, std::size(cases));
}

FIC2_TEST(InfoPresenter_ExistingFormatLabelsAreUnchanged)
{
    const Expected cases[] = {
        { DXGI_FORMAT_R8G8B8A8_UNORM, L"R8G8B8A8", 32 },
        { DXGI_FORMAT_B8G8R8A8_UNORM, L"B8G8R8A8", 32 },
        { DXGI_FORMAT_B8G8R8X8_UNORM, L"B8G8R8X8", 32 },
        { DXGI_FORMAT_R16G16B16A16_UNORM, L"R16G16B16A16", 64 },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, L"R16G16B16A16F", 64 },
        { DXGI_FORMAT_R32G32B32A32_FLOAT, L"R32G32B32A32F", 128 },
        { DXGI_FORMAT_R10G10B10A2_UNORM, L"R10G10B10A2", 32 },
        { DXGI_FORMAT_B5G6R5_UNORM, L"B5G6R5", 16 },
        { DXGI_FORMAT_B5G5R5A1_UNORM, L"B5G5R5A1", 16 },
        { DXGI_FORMAT_BC1_UNORM, L"BC1", 4 },
        { DXGI_FORMAT_BC2_UNORM, L"BC2", 8 },
        { DXGI_FORMAT_BC3_UNORM, L"BC3", 8 },
        { DXGI_FORMAT_BC4_UNORM, L"BC4", 4 },
        { DXGI_FORMAT_BC5_UNORM, L"BC5", 8 },
        { DXGI_FORMAT_BC6H_UF16, L"BC6H", 8 },
        { DXGI_FORMAT_BC7_UNORM, L"BC7", 8 },
    };
    CheckLabels(cases, std::size(cases));
}

FIC2_TEST(InfoPresenter_UnlistedFormatStillReportsUnknown)
{
    const std::wstring info = InfoFor(DXGI_FORMAT_R8_UNORM);
    FIC2_CHECK(StartsWith(info, L"256 x 128 x ? | Unknown"));
}
