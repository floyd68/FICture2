#pragma once

#include <cstdint>

namespace ImageBrowserThumbLayoutCoordinator
{
    struct ThumbSizeInput
    {
        float paneHeight { 0.0f };
        float currentThumbHeight { 0.0f };
        bool splitterDragging { false };
        unsigned long long nowMs { 0 };
        unsigned long long lastApplyMs { 0 };
        float thumbMinSide { 32.0f };
        float thumbMaxSide { 256.0f };
        float contentPadding { 4.0f };
    };

    struct ThumbSizeResult
    {
        bool shouldApply { false };
        float newThumbSide { 0.0f };
        unsigned long long updatedApplyMs { 0 };
    };

    struct SyncedSplitRatioInput
    {
        float totalHeight { 0.0f };
        float syncedHeight { 0.0f };
        float thumbStripMinH { 0.0f };
        float thumbStripMaxH { 0.0f };
        float splitterHitThickness { 12.0f };
    };

    struct SyncedSplitRatioResult
    {
        bool valid { false };
        float desiredSecondHeight { 0.0f };
        float splitRatio { 0.0f };
    };

    ThumbSizeResult EvaluateThumbSize(const ThumbSizeInput& input);
    float ClampThumbStripHeight(float measuredHeight, float thumbStripMinH, float thumbStripMaxH);
    SyncedSplitRatioResult ComputeSyncedSplitRatio(const SyncedSplitRatioInput& input);
}
