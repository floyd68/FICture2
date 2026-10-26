#include "ImageBrowserThumbLayout.h"

#include <algorithm>

void ThumbStripLayout::Reset(std::vector<float> extents, float spacing, float padding)
{
    m_extents = std::move(extents);
    m_spacing = spacing;
    m_padding = padding;
    m_starts.assign(m_extents.size(), 0.0f);
    m_validStarts = 0;
}

void ThumbStripLayout::SetExtent(size_t index, float extent)
{
    if (index >= m_extents.size() || m_extents[index] == extent)
    {
        return;
    }
    m_extents[index] = extent;
    // Starts up to and including `index` are unchanged.
    m_validStarts = (std::min)(m_validStarts, index + 1);
}

void ThumbStripLayout::EnsureStarts() const
{
    if (m_validStarts == m_extents.size())
    {
        return;
    }
    size_t i = m_validStarts;
    float x = (i == 0) ? m_padding : m_starts[i - 1] + m_extents[i - 1] + m_spacing;
    for (; i < m_extents.size(); ++i)
    {
        m_starts[i] = x;
        x += m_extents[i] + m_spacing;
    }
    m_validStarts = m_extents.size();
}

float ThumbStripLayout::Start(size_t index) const
{
    EnsureStarts();
    return m_starts[index];
}

float ThumbStripLayout::ContentWidth() const
{
    if (m_extents.empty())
    {
        return 2.0f * m_padding;
    }
    return End(m_extents.size() - 1) + m_padding;
}

std::pair<size_t, size_t> ThumbStripLayout::Range(float left, float right) const
{
    if (m_extents.empty() || right <= left)
    {
        return { 0, 0 };
    }
    EnsureStarts();
    // First item ending after `left`: ends are increasing because extents are >= 0.
    size_t lo = 0;
    size_t hi = m_extents.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_starts[mid] + m_extents[mid] <= left)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    const size_t first = lo;
    // First item starting at or after `right`.
    const auto it = std::lower_bound(m_starts.begin() + static_cast<std::ptrdiff_t>(first), m_starts.end(), right);
    const size_t last = static_cast<size_t>(it - m_starts.begin());
    return { first, (std::max)(first, last) };
}

std::pair<size_t, size_t> ThumbStripLayout::MaterializeRange(float left, float right, size_t maxMargin) const
{
    const auto [first, last] = Range(left, right);
    const size_t margin = (std::min)(last - first, maxMargin);
    return { first - (std::min)(first, margin), (std::min)(m_extents.size(), last + margin) };
}

std::pair<size_t, size_t> ThumbStripLayout::RangeAround(size_t index, size_t margin) const
{
    const size_t count = m_extents.size();
    if (count == 0)
    {
        return { 0, 0 };
    }
    index = (std::min)(index, count - 1);
    return { index - (std::min)(index, margin), (std::min)(count, index + margin + 1) };
}

ThumbStripLayout::Anchor ThumbStripLayout::AnchorFor(float scroll, float viewportWidth, size_t selected) const
{
    Anchor anchor {};
    const auto [first, last] = Range(scroll, scroll + viewportWidth);
    if (first == last)
    {
        return anchor;
    }
    anchor.valid = true;
    anchor.index = (selected >= first && selected < last) ? selected : first;
    anchor.offset = Start(anchor.index) - scroll;
    return anchor;
}

float ThumbStripLayout::ScrollKeeping(const Anchor& anchor) const
{
    if (!anchor.valid || anchor.index >= m_extents.size())
    {
        return 0.0f;
    }
    return Start(anchor.index) - anchor.offset;
}
