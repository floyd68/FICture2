#include "ImageBrowserDragOverlay.h"

ImageBrowserDragOverlay::ImageBrowserDragOverlay(ID2D1RenderTarget* target)
{
    target->CreateSolidColorBrush( D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.18f), m_replaceBrush.ReleaseAndGetAddressOf());
    target->CreateSolidColorBrush( D2D1::ColorF(0.0f, 1.0f, 0.0f, 0.18f), m_insertBrush.ReleaseAndGetAddressOf());
}
void ImageBrowserDragOverlay::Draw(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, Kind kind)
{
    if (!target || kind == Kind::None)
    {
        return;
    }

    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    if (kind == Kind::Replace)
    {
        target->FillRectangle(rect, m_replaceBrush.Get());
    }
    else
    {
        const float w = (std::max)(1.0f, rect.right - rect.left);
        const float splitX = rect.left + (w * 0.75f);
        const D2D1_RECT_F rr { splitX, rect.top, rect.right, rect.bottom };
        target->FillRectangle(rr, m_insertBrush.Get());
    }
}
