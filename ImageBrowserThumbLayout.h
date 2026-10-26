#pragma once

#include <cstddef>
#include <string>
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

    // Prefetch margin of the materialized tile range, counted in items: one
    // viewport's worth (the visible item count) on each side, at most
    // kMaxPrefetchItems per side. Counting items rather than pixels keeps the
    // cap whether or not decoded aspects are known yet.
    static constexpr size_t kMaxPrefetchItems = 32;

    // The items to materialize for the viewport [left, right): Range(left,
    // right) plus min(visible count, maxMargin) items on each side, clamped to
    // the model; at most visible + 2 * maxMargin items.
    std::pair<size_t, size_t> MaterializeRange(float left, float right, size_t maxMargin = kMaxPrefetchItems) const;
    // Without a viewport (before the first layout pass): `index` and up to
    // `margin` items on each side, clamped to the model.
    std::pair<size_t, size_t> RangeAround(size_t index, size_t margin = kMaxPrefetchItems) const;

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

    // After the list is rebuilt: center the selection only when it is a
    // different (or newly resolved) item. An unchanged selection - including a
    // final progressive update that re-applies it - keeps the captured anchor.
    static bool CenterAfterRebuild(const std::wstring& previousSelectedKey, const std::wstring& selectedKey, bool hasItems)
    {
        return hasItems && selectedKey != previousSelectedKey;
    }

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
