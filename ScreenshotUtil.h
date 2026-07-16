#pragma once

#include <d2d1.h>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace ScreenshotUtil
{
    // Capture a client-space rectangle from hwnd into a PNG file.
    bool SaveClientRectPng(HWND hwnd, const D2D1_RECT_F& clientRect, const std::wstring& pngPath);

    // "Save As" dialog filtered to *.png.
    bool ShowSavePngDialog(
        HWND owner,
        const std::wstring& initialFolder,
        const std::wstring& initialFileName,
        std::wstring& outPath);
}
