#pragma once

#include <d2d1.h>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace FD2D
{
class Backplate;
}

namespace ScreenshotUtil
{
    // Capture a logical client-space rectangle from the last composed offscreen
    // frame into a PNG. Call after ensuring the backplate has a current frame
    // (this helper invokes Render()). Independent of desktop occlusion; requires
    // the D3D + offscreen-buffer path (fails for D2D-only backends).
    bool SaveRenderSurfaceRectPng(
        FD2D::Backplate& backplate,
        const D2D1_RECT_F& logicalRect,
        const std::wstring& pngPath);

    // "Save As" dialog filtered to *.png.
    bool ShowSavePngDialog(
        HWND owner,
        const std::wstring& initialFolder,
        const std::wstring& initialFileName,
        std::wstring& outPath);
}
