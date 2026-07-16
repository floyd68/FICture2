#pragma once

#include <Windows.h>

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
    };

    void Configure(HMENU hPopup, const ConfigurePayload& payload);
    UINT TrackAndReturnCommand(HMENU hPopup, HWND hwnd, const POINT& clientPt);
}
