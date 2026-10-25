#pragma once

#include <cstddef>
#include <utility>
#include <vector>

// Horizontal geometry of the thumbnail strip, computed from item extents
// instead of tile windows. Item i occupies [Start(i), End(i)) in panel
// coordinates (0 is the panel's left edge; the panel padding comes first and
// items are separated by the spacing). Extents can change one at a time as
// decoded aspect ratios arrive; offsets are rebuilt lazily from the first
// changed item.
//
// Width estimation: an image's extent is its row height times its aspect
// ratio once decoded, and the row height (square) before that.
class ThumbStripLayout
{
public:
    void Reset(std::vector<float> extents, float spacing, float padding);
    void SetExtent(size_t index, float extent);

    size_t Count() const { return m_extents.size(); }
    float Extent(size_t index) const { return m_extents[index]; }
    float Start(size_t index) const;
    float End(size_t index) const { return Start(index) + m_extents[index]; }
    // Padding on both sides plus every extent and the spacing between them.
    float ContentWidth() const;

    // Items overlapping [left, right) in panel coordinates, as [first, last).
    std::pair<size_t, size_t> Range(float left, float right) const;

    // The item to keep in place when extents change: the selection when it is
    // at least partly visible, otherwise the first visible item. `offset` is
    // its start relative to the viewport's left edge.
    struct Anchor
    {
        bool valid { false };
        size_t index { 0 };
        float offset { 0.0f };
    };
    Anchor AnchorFor(float scroll, float viewportWidth, size_t selected) const;
    // The scroll offset that puts the anchor back at its viewport offset.
    float ScrollKeeping(const Anchor& anchor) const;

    static float ImageExtent(float rowHeight, float aspect, float margin)
    {
        const float width = (aspect > 0.0f) ? rowHeight * aspect : rowHeight;
        return width + 2.0f * margin;
    }

private:
    void EnsureStarts() const;

    std::vector<float> m_extents {};
    float m_spacing { 0.0f };
    float m_padding { 0.0f };
    mutable std::vector<float> m_starts {};
    mutable size_t m_validStarts { 0 };   // m_starts[0, m_validStarts) are current
};
