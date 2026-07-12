#pragma once

#include <d2d1.h>

namespace ImageViewUtil
{
    // Applies zoom (around the rect center) and pan offset to a rect.
    // When zoomScale == 1, only a pan larger than the epsilon is applied.
    // panX/panY must already be in the same coordinate space as the rect.
    D2D1_RECT_F ApplyZoomPanToRect(
        const D2D1_RECT_F& base,
        float zoomScale,
        float panX,
        float panY);
}
