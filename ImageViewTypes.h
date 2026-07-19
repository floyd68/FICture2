#pragma once

#include "ImageCore/DecodedImage.h"

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
    // 0=RGBA, 1=R, 2=G, 3=B, 4=A (D3D presentation path).
    int channelMode { 0 };
    uint32_t mipLevel { 0 };
};

struct ImageLoadedInfo
{
    uint32_t width { 0 };
    uint32_t height { 0 };
    DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
    uint32_t sourceMipLevels { 1 };
    uint32_t sourceMipIndex { 0 };
    uint32_t sourceWidth { 0 };
    uint32_t sourceHeight { 0 };
    ImageCore::AlphaEncoding alphaEncoding { ImageCore::AlphaEncoding::Unknown };
    ImageCore::AlphaUsage alphaUsageHint { ImageCore::AlphaUsage::Auto };
    ImageCore::AlphaUsage alphaUsageOverride { ImageCore::AlphaUsage::Auto };
    ImageCore::AlphaUsage effectiveAlphaUsage { ImageCore::AlphaUsage::Auto };
    bool sourceWasBlockCompressed { false };
    bool gpuPresentation { false };
    int channelMode { 0 };
    bool alphaCheckerboardEnabled { false };
    bool highQualitySampling { true };
    float zoomScale { 1.0f };
    float panX { 0.0f };
    float panY { 0.0f };
    int rotationQuarters { 0 };
    std::wstring sourcePath {};
};
