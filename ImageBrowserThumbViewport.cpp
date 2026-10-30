#include "ImageBrowserThumbViewport.h"

#include <algorithm>
#include <utility>

bool ThumbStripViewport::Ready(const ThumbItemModel& model) const
{
    return model.DisplayCount() > 0 && m_layout.Count() == model.DisplayCount();
}

void ThumbStripViewport::Rebuild(std::vector<float> extents, float spacing, float padding)
{
    m_layout.Reset(std::move(extents), spacing, padding);
    m_arrangePending = true;
}

bool ThumbStripViewport::SetExtent(const ThumbItemModel& model, size_t index, float extent,
    const std::optional<ThumbViewportSpan>& viewport, size_t selected)
{
    if (!Ready(model) || index >= m_layout.Count())
    {
        return false;
    }
    CaptureAnchor(model, viewport, selected);
    m_layout.SetExtent(index, extent);
    m_arrangePending = true;
    return true;
}

void ThumbStripViewport::CaptureAnchor(const ThumbItemModel& model,
    const std::optional<ThumbViewportSpan>& viewport, size_t selected)
{
    if (viewport)
    {
        m_scroll.CaptureAnchor(model, m_layout, viewport->left, viewport->width, selected);
    }
}

ThumbStripScroll::Adjustment ThumbStripViewport::TakeAfterArrange(const ThumbItemModel& model,
    const std::optional<ThumbViewportSpan>& viewport)
{
    m_arrangePending = false;
    if (!viewport)
    {
        return {};
    }
    return m_scroll.Take(model, m_layout, viewport->left, viewport->width);
}

ThumbRanges ThumbStripViewport::Ranges(const ThumbItemModel& model, const std::optional<ThumbViewportSpan>& viewport,
    size_t selected, ThumbViewportBasis basis) const
{
    ThumbRanges ranges {};
    if (!Ready(model))
    {
        return ranges;
    }
    if (!viewport)
    {
        const auto [first, last] = m_layout.RangeAround((selected < model.DisplayCount()) ? selected : 0);
        ranges.materialized = { first, last };
        return ranges;
    }
    float left = viewport->left;
    const float width = viewport->width;
    if (basis == ThumbViewportBasis::Expected)
    {
        left = m_scroll.ExpectedLeft(model, m_layout, left, width, selected);
    }
    const auto [first, last] = m_layout.MaterializeRange(left, left + width);
    const auto [visibleFirst, visibleLast] = m_layout.Range(left, left + width);
    ranges.materialized = { first, last };
    ranges.visible = { visibleFirst, visibleLast };
    return ranges;
}

std::optional<ThumbIndexRange> ThumbStripViewport::VisibleRange(const ThumbItemModel& model,
    const std::optional<ThumbViewportSpan>& viewport) const
{
    if (!Ready(model) || !viewport)
    {
        return std::nullopt;
    }
    const auto [first, last] = m_layout.Range(viewport->left, viewport->left + viewport->width);
    return ThumbIndexRange { first, last };
}

std::optional<size_t> ThumbStripViewport::FullyVisibleCount(const ThumbItemModel& model,
    const std::optional<ThumbViewportSpan>& viewport) const
{
    const auto visible = VisibleRange(model, viewport);
    if (!visible)
    {
        return std::nullopt;
    }
    const float left = viewport->left;
    const float right = viewport->left + viewport->width;
    size_t full = 0;
    for (size_t i = visible->first; i < visible->last; ++i)
    {
        if (m_layout.Start(i) >= left - 0.5f && m_layout.End(i) <= right + 0.5f)
        {
            ++full;
        }
    }
    return (std::max)(static_cast<size_t>(1), full);
}
