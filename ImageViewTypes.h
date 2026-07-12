#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <string>

// App-owned image view / load metadata (moved out of FD2D::Image for decoupling).
struct ImageViewTransform
{
    float zoomScale { 1.0f };
    float targetZoomScale { 1.0f };
    float zoomVelocity { 0.0f };
    float panX { 0.0f };
    float panY { 0.0f };
    // 0 = 0°, 1 = 90°CW, 2 = 180°, 3 = 270°CW.
    int rotationQuarters { 0 };
};

struct ImageLoadedInfo
{
    uint32_t width { 0 };
    uint32_t height { 0 };
    DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
    std::wstring sourcePath {};
};
