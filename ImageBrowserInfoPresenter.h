#pragma once

#include "ImageBrowserThumbTypes.h"
#include "ImageViewTypes.h"
#include "VirtualPath.h"

#include <string>

namespace ImageBrowserInfoPresenter
{
    struct Input
    {
        std::wstring activePath {};
        bool hasSelection { false };
        ThumbItemKind selectedKind { ThumbItemKind::Image };
        Floar::VirtualPath selectedPath {};
        Floar::VirtualPath currentFolder {};

        bool hasLoadedInfo { false };
        ImageLoadedInfo loadedInfo {};

        int zoomPercent { 100 };
        // 0 = 0°, 1 = 90°CW, 2 = 180°, 3 = 270°CW.
        int rotationQuarters { 0 };
        bool hasSamplingState { false };
        bool highQualitySampling { true };
        bool useD3DRenderer { false };
    };

    struct Output
    {
        std::wstring pathText {};
        std::wstring infoText {};
        std::wstring zoomText {};
    };

    Output Build(const Input& input);
}
