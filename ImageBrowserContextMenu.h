#pragma once

#include "ImageCore/DecodedImage.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ImageBrowserContextMenu
{
    struct ConfigurePayload
    {
        int viewerCount { 1 };
        bool canClose { false };
        bool showNavItems { true };
        bool showAlpha { true };
        std::wstring samplingLabel {};
        bool hasExplorerTarget { false };
        bool canSaveScreenshot { false };
        bool thumbRegistered { false };
        bool associationsRegistered { false };
        std::vector<std::wstring> recentFiles {};

        bool hasImage { false };
        ImageCore::AlphaUsage alphaUsageOverride { ImageCore::AlphaUsage::Auto };
        uint32_t mipLevels { 1 };
        uint32_t mipLevel { 0 };
        uint32_t sourceWidth { 0 };
        uint32_t sourceHeight { 0 };
    };

    void Configure(HMENU hPopup, const ConfigurePayload& payload);
    UINT TrackAndReturnCommand(HMENU hPopup, HWND hwnd, const POINT& clientPt);
}
