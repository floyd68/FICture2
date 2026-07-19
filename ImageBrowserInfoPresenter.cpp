#include "ImageBrowserInfoPresenter.h"

#include "ArchiveBadge.h"
#include "VirtualPath.h"
#include "CommonUtil.h"
#include "FD2D/Core.h"

#include <algorithm>
#include <cwctype>

namespace
{
    const wchar_t* DxgiFormatToString(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return L"R8G8B8A8";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return L"B8G8R8X8";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return L"R16G16B16A16";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16F";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return L"R32G32B32A32F";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return L"R10G10B10A2";
        case DXGI_FORMAT_B5G6R5_UNORM: return L"B5G6R5";
        case DXGI_FORMAT_B5G5R5A1_UNORM: return L"B5G5R5A1";
        case DXGI_FORMAT_BC1_UNORM: return L"BC1";
        case DXGI_FORMAT_BC2_UNORM: return L"BC2";
        case DXGI_FORMAT_BC3_UNORM: return L"BC3";
        case DXGI_FORMAT_BC4_UNORM: return L"BC4";
        case DXGI_FORMAT_BC5_UNORM: return L"BC5";
        case DXGI_FORMAT_BC6H_UF16: return L"BC6H";
        case DXGI_FORMAT_BC7_UNORM: return L"BC7";
        default: return L"Unknown";
        }
    }

    int DxgiBitsPerPixel(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return 128;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return 64;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 32;
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
            return 16;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC4_UNORM:
            return 4;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC7_UNORM:
            return 8;
        default:
            return 0;
        }
    }

    std::wstring ArchiveFormatLabelForPath(const std::wstring& path)
    {
        auto vpath = Floar::VirtualPath::Parse(path);
        if (!vpath)
        {
            return L"";
        }

        if (!vpath->IsInArchive() && !vpath->IsArchiveFile())
        {
            return L"";
        }

        const std::wstring hostExt = CommonUtil::ToLower(vpath->hostPath.extension().wstring());
        return ArchiveBadge::BadgeLabelForExt(hostExt);
    }
}

ImageBrowserInfoPresenter::Output ImageBrowserInfoPresenter::Build(const Input& input)
{
    Output output {};
    std::wstring samplingLabel {};

    std::wstring displayedFullPath = input.activePath;
    if (input.hasLoadedInfo && !input.loadedInfo.sourcePath.empty())
    {
        displayedFullPath = input.loadedInfo.sourcePath;
    }

    const bool isFolderSelected =
        input.hasSelection &&
        (input.selectedKind == ThumbItemKind::Folder || input.selectedKind == ThumbItemKind::Up);
    if (isFolderSelected)
    {
        if (input.selectedKind == ThumbItemKind::Up && !input.currentFolder.empty())
        {
            displayedFullPath = input.currentFolder.GetDisplayPath();
        }
        else
        {
            displayedFullPath = input.selectedPath.GetDisplayPath();
        }
    }

    output.pathText = displayedFullPath.empty() ? L"-" : displayedFullPath;
    const std::wstring archiveLabel = ArchiveFormatLabelForPath(output.pathText);

    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (!isFolderSelected && input.hasLoadedInfo)
    {
        width = input.loadedInfo.width;
        height = input.loadedInfo.height;
        format = input.loadedInfo.format;
    }

    if (isFolderSelected)
    {
        output.infoText = archiveLabel.empty() ? L"-" : archiveLabel;
    }
    else
    {
        std::wstring dimensions = L"-";
        if (width > 0 && height > 0)
        {
            const int bpp = DxgiBitsPerPixel(format);
            if (bpp > 0)
            {
                dimensions = std::to_wstring(width) + L" x " + std::to_wstring(height) + L" x " + std::to_wstring(bpp);
            }
            else
            {
                dimensions = std::to_wstring(width) + L" x " + std::to_wstring(height) + L" x ?";
            }
        }

        output.infoText.reserve(256);
        output.infoText += dimensions;
        output.infoText += L" | ";
        output.infoText += DxgiFormatToString(format);
        if (input.hasLoadedInfo && input.loadedInfo.sourceWasBlockCompressed)
        {
            output.infoText += L" | BC";
        }
        if (input.loadedInfo.sourceMipLevels > 1)
        {
            output.infoText += L" | Mip ";
            output.infoText += std::to_wstring(input.loadedInfo.sourceMipIndex);
            output.infoText += L" / ";
            output.infoText += std::to_wstring(input.loadedInfo.sourceMipLevels - 1);
            output.infoText += L" (";
            output.infoText += std::to_wstring(input.loadedInfo.sourceMipLevels);
            output.infoText += L" levels)";
            if (input.loadedInfo.sourceMipIndex != 0 &&
                input.loadedInfo.sourceWidth > 0 &&
                input.loadedInfo.sourceHeight > 0)
            {
                output.infoText += L" | Source ";
                output.infoText += std::to_wstring(input.loadedInfo.sourceWidth);
                output.infoText += L" x ";
                output.infoText += std::to_wstring(input.loadedInfo.sourceHeight);
            }
        }
        if (input.hasLoadedInfo && width > 0 && height > 0)
        {
            static const wchar_t* kChannelNames[] = { L"RGBA", L"R", L"G", L"B", L"A" };
            const int channel = (std::max)(0, (std::min)(4, input.loadedInfo.channelMode));
            output.infoText += L" | ";
            output.infoText += kChannelNames[channel];

            auto alphaName = [](ImageCore::AlphaUsage usage) -> const wchar_t*
            {
                switch (usage)
                {
                case ImageCore::AlphaUsage::Coverage: return L"Transparency";
                case ImageCore::AlphaUsage::Data: return L"Opaque";
                case ImageCore::AlphaUsage::Auto: return L"Auto";
                default: return L"Unknown";
                }
            };
            output.infoText += L" | Alpha ";
            output.infoText += alphaName(input.loadedInfo.effectiveAlphaUsage);
            if (input.loadedInfo.alphaUsageOverride == ImageCore::AlphaUsage::Auto)
            {
                output.infoText += L" (Auto)";
            }
            else
            {
                output.infoText += L" (Override)";
            }
        }
        if (!archiveLabel.empty())
        {
            output.infoText += L" | ";
            output.infoText += archiveLabel;
        }
        if (input.hasSamplingState)
        {
            if (input.useD3DRenderer)
            {
                samplingLabel = input.highQualitySampling ? L"D3D11 Anisotropic" : L"D3D11 Point";
            }
            else
            {
                const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
                if (input.highQualitySampling)
                {
                    samplingLabel = (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D HQ Cubic" : L"D2D Linear";
                }
                else
                {
                    samplingLabel = (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D Nearest" : L"D2D Linear";
                }
            }
        }

        if (!samplingLabel.empty())
        {
            output.infoText += L" | ";
            output.infoText += samplingLabel;
        }
    }

    output.zoomText = std::to_wstring(input.zoomPercent) + L"%";
    const int quarters = ((input.rotationQuarters % 4) + 4) % 4;
    if (quarters != 0)
    {
        output.zoomText += L" ";
        output.zoomText += std::to_wstring(quarters * 90);
        output.zoomText += L"\u00B0";
    }
    return output;
}
