#include "ImageBrowserThumbnailPane.h"
#include "FD2D/Util.h"

#include <algorithm>
#include <cmath>

ImageBrowserThumbnailPane::ImageBrowserThumbnailPane()
    : FD2D::Wnd(L"thumbnailPane")
{
}

void ImageBrowserThumbnailPane::Build(
    const std::shared_ptr<FD2D::SplitPanel>& rootSplit,
    const std::function<void(int)>& onWheelSteps,
    const std::function<void()>& onWheelFocus)
{
    if (!rootSplit)
    {
        return;
    }

    m_onWheelSteps = onWheelSteps;
    m_onWheelFocus = onWheelFocus;
    m_wheelRemainder = 0;

    m_thumbPanel = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
    m_thumbPanel->SetSpacing(4.0f);
    m_thumbPanel->SetPadding(4.0f);

    m_thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
    m_thumbScroll->SetScrollStep(96.0f);
    m_thumbScroll->SetSmoothTimeMs(110);
    m_thumbScroll->SetVerticalScrollEnabled(false);
    m_thumbScroll->SetContent(m_thumbPanel);

    ClearChildren();
    AddChild(m_thumbScroll);
    rootSplit->SetSecondChild(shared_from_this());
}

bool ImageBrowserThumbnailPane::TryGetStripRect(D2D1_RECT_F& outRect) const
{
    if (m_thumbScroll == nullptr)
    {
        return false;
    }

    outRect = m_thumbScroll->LayoutRect();
    return outRect.right > outRect.left && outRect.bottom > outRect.top;
}

size_t ImageBrowserThumbnailPane::PagingStep(float itemExtent) const
{
    D2D1_RECT_F stripRect {};
    if (!TryGetStripRect(stripRect))
    {
        return 1;
    }

    const float width = stripRect.right - stripRect.left;
    if (width <= 1.0f)
    {
        return 1;
    }

    const float clampedExtent = (std::max)(1.0f, itemExtent);
    const int count = static_cast<int>(std::floor(width / clampedExtent));
    return static_cast<size_t>((std::max)(1, count));
}

bool ImageBrowserThumbnailPane::OnInputEvent(const FD2D::InputEvent& event)
{
    if (event.type == FD2D::InputEventType::MouseWheel && event.hasPoint && m_thumbScroll != nullptr)
    {
        const D2D1_RECT_F stripRect = m_thumbScroll->LayoutRect();
        if (FD2D::Util::RectContainsPoint(stripRect, event.point))
        {
            if (m_onWheelFocus)
            {
                m_onWheelFocus();
            }

            m_wheelRemainder += event.wheelDelta;
            const int steps = m_wheelRemainder / WHEEL_DELTA;
            m_wheelRemainder = m_wheelRemainder % WHEEL_DELTA;
            if (steps != 0 && m_onWheelSteps)
            {
                m_onWheelSteps(steps);
            }
            return true;
        }
    }

    return Wnd::OnInputEvent(event);
}
