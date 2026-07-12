#include "ImageViewUtil.h"

#include <cmath>

namespace ImageViewUtil
{
    D2D1_RECT_F ApplyZoomPanToRect(
        const D2D1_RECT_F& base,
        float zoomScale,
        float panX,
        float panY)
    {
        D2D1_RECT_F rect = base;
        if (zoomScale != 1.0f)
        {
            const float centerX = (base.left + base.right) * 0.5f;
            const float centerY = (base.top + base.bottom) * 0.5f;
            const float scaledWidth = (base.right - base.left) * zoomScale;
            const float scaledHeight = (base.bottom - base.top) * zoomScale;
            rect.left = centerX - scaledWidth * 0.5f + panX;
            rect.right = rect.left + scaledWidth;
            rect.top = centerY - scaledHeight * 0.5f + panY;
            rect.bottom = rect.top + scaledHeight;
        }
        else if (std::abs(panX) > 0.001f || std::abs(panY) > 0.001f)
        {
            // Apply pan even when not zoomed (though this shouldn't normally happen).
            rect.left += panX;
            rect.right += panX;
            rect.top += panY;
            rect.bottom += panY;
        }
        return rect;
    }
}
