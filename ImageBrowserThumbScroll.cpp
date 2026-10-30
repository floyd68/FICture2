#include "ImageBrowserThumbScroll.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    bool LayoutMatches(const ThumbItemModel& model, const ThumbStripLayout& layout)
    {
        return model.DisplayCount() > 0 && layout.Count() == model.DisplayCount();
    }
}

void ThumbStripScroll::CaptureAnchor(const ThumbItemModel& model, const ThumbStripLayout& layout,
    float left, float width, size_t selected)
{
    if (m_anchor.valid || m_centerPending || !LayoutMatches(model, layout))
    {
        return;
    }
    const ThumbStripLayout::Anchor anchor = layout.AnchorFor(left, width, selected);
    if (anchor.valid)
    {
        m_anchor = { true, model.At(anchor.index).lookupKey, anchor.offset };
    }
}

float ThumbStripScroll::ExpectedLeft(const ThumbItemModel& model, const ThumbStripLayout& layout,
    float left, float width, size_t selected) const
{
    if (!LayoutMatches(model, layout))
    {
        return left;
    }
    float expected = left;
    if (m_centerPending && selected < model.DisplayCount())
    {
        expected = 0.5f * (layout.Start(selected) + layout.End(selected) - width);
    }
    else if (m_anchor.valid)
    {
        if (const auto index = model.IndexOf(m_anchor.key))
        {
            expected = layout.Start(*index) - m_anchor.offset;
        }
    }
    const float maxLeft = (std::max)(0.0f, layout.ContentWidth() - width);
    return (std::min)((std::max)(expected, 0.0f), maxLeft);
}

ThumbStripScroll::Adjustment ThumbStripScroll::Take(const ThumbItemModel& model, const ThumbStripLayout& layout,
    float left, float width)
{
    Adjustment adjustment {};
    if (!LayoutMatches(model, layout))
    {
        return adjustment;
    }
    if (std::fabs(width - m_lastViewportWidth) > 0.5f)
    {
        m_lastViewportWidth = width;
        m_centerPending = true;
    }
    if (m_centerPending)
    {
        m_centerPending = false;
        m_anchor = {};
        adjustment.kind = Adjustment::Kind::CenterSelection;
        return adjustment;
    }
    if (m_anchor.valid)
    {
        const Anchor anchor = std::exchange(m_anchor, {});
        if (const auto index = model.IndexOf(anchor.key))
        {
            adjustment.kind = Adjustment::Kind::Shift;
            adjustment.shift = (layout.Start(*index) - anchor.offset) - left;
        }
    }
    return adjustment;
}
