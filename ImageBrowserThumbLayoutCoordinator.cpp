#include "ImageBrowserThumbLayoutCoordinator.h"

#include <algorithm>
#include <cmath>

ImageBrowserThumbLayoutCoordinator::ThumbSizeResult
ImageBrowserThumbLayoutCoordinator::EvaluateThumbSize(const ThumbSizeInput& input)
{
    ThumbSizeResult result {};
    if (input.paneHeight <= 1.0f)
    {
        return result;
    }

    const float availableForThumb = input.paneHeight - (input.contentPadding * 2.0f);
    float newHeight = (std::max)(input.thumbMinSide, (std::min)(input.thumbMaxSide, availableForThumb));
    newHeight = std::round(newHeight);

    const float minDelta = input.splitterDragging ? 1.5f : 0.5f;
    if (input.splitterDragging)
    {
        if ((input.nowMs - input.lastApplyMs) < 33ULL &&
            std::abs(newHeight - input.currentThumbHeight) < 6.0f)
        {
            return result;
        }
    }

    if (std::abs(newHeight - input.currentThumbHeight) < minDelta)
    {
        return result;
    }

    result.shouldApply = true;
    result.newThumbSide = newHeight;
    result.updatedApplyMs = input.nowMs;
    return result;
}

float ImageBrowserThumbLayoutCoordinator::ClampThumbStripHeight(
    float measuredHeight,
    float thumbStripMinH,
    float thumbStripMaxH)
{
    return (std::max)(thumbStripMinH, (std::min)(thumbStripMaxH, measuredHeight));
}

ImageBrowserThumbLayoutCoordinator::SyncedSplitRatioResult
ImageBrowserThumbLayoutCoordinator::ComputeSyncedSplitRatio(const SyncedSplitRatioInput& input)
{
    SyncedSplitRatioResult result {};
    if (input.totalHeight <= 0.0f)
    {
        return result;
    }

    const float desiredSecond = ClampThumbStripHeight(input.syncedHeight, input.thumbStripMinH, input.thumbStripMaxH);
    const float availableH = (std::max)(1.0f, input.totalHeight - input.splitterHitThickness);
    const float ratio = 1.0f - (desiredSecond / availableH);

    result.valid = true;
    result.desiredSecondHeight = desiredSecond;
    result.splitRatio = (std::max)(0.0f, (std::min)(1.0f, ratio));
    return result;
}
