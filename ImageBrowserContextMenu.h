#pragma once

#include <Windows.h>

#include <string>

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
        bool thumbRegistered { false };
    };

    void Configure(HMENU hPopup, const ConfigurePayload& payload);
    UINT TrackAndReturnCommand(HMENU hPopup, HWND hwnd, const POINT& clientPt);
}
