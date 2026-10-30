#include "ImageBrowserThumbnailPane.h"
#include "FD2D/Util.h"
#include "SelectionStyle.h"

#include <algorithm>
#include <cmath>

static_assert(ImageBrowserThumbnailPane::kSelectionRoom >= FD2D::SelectionMaxOutset(FD2D::SelectionStyle {}),
    "the strip must leave room for the default selection outline");

ThumbStripPanel::ThumbStripPanel()
    : FD2D::StackPanel(L"thumbs", FD2D::Orientation::Horizontal)
{
}

FD2D::Size ThumbStripPanel::Measure(FD2D::Size available)
{
    // The extent comes from the layout and the height from the row height, so
    // the strip keeps its size whichever tiles (if any) are materialized.
    UNREFERENCED_PARAMETER(available);
    const float content = (m_layout != nullptr) ? m_layout->ContentWidth() : 2.0f * m_padding;
    constexpr float room = ImageBrowserThumbnailPane::kSelectionRoom;
    m_desired = { content + 2.0f * m_margin, m_rowHeight + 2.0f * room + 2.0f * m_margin };
    return m_desired;
}

void ThumbStripPanel::Arrange(FD2D::Rect finalRect)
{
    // Tiles sit kSelectionRoom below the top, at the row height, so their
    // selection outlines stay inside the panel (the scroll view arranges it
    // above the scrollbar gutter).
    constexpr float room = ImageBrowserThumbnailPane::kSelectionRoom;
    const FD2D::Rect inset = FD2D::Inset(finalRect, m_margin);
    const float available = (std::max)(0.0f, inset.h - 2.0f * room);
    const float rowHeight = (m_rowHeight > 0.0f) ? (std::min)(m_rowHeight, available) : available;
    if (m_layout != nullptr && m_tiles != nullptr)
    {
        for (const ThumbTile& tile : m_tiles->Tiles())
        {
            if (tile.focus && tile.index < m_layout->Count())
            {
                tile.focus->Arrange({ inset.x + m_layout->Start(tile.index), inset.y + room, m_layout->Extent(tile.index), rowHeight });
            }
        }
    }
    m_layoutRect = FD2D::ToD2D(finalRect);
}

void ThumbStripPanel::OnRender(ID2D1RenderTarget* target)
{
    if (target != nullptr && m_onBeforeRenderTiles)
    {
        m_onBeforeRenderTiles();
    }
    FD2D::StackPanel::OnRender(target);
}

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

    m_thumbPanel = std::make_shared<ThumbStripPanel>();
    m_thumbPanel->SetSpacing(kItemSpacing);
    m_thumbPanel->SetPadding(kPanelPadding);

    m_thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
    m_thumbScroll->SetScrollStep(96.0f);
    m_thumbScroll->SetSmoothTimeMs(110);
    m_thumbScroll->SetVerticalScrollEnabled(false);
    m_thumbScroll->SetScrollBarsVisible(true); // horizontal bar when the strip overflows
    m_thumbScroll->SetScrollBarGutter(true);   // drawn below the tiles, never over them
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
