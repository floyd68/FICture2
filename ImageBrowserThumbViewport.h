#pragma once

#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbScroll.h"
#include "ImageBrowserThumbTypes.h"

#include <cstddef>
#include <optional>
#include <vector>

// The strip viewport in panel coordinates: [left, left + width).
struct ThumbViewportSpan
{
    float left { 0.0f };
    float width { 0.0f };
};

struct ThumbRanges
{
    ThumbIndexRange materialized {};   // items that have tiles
    ThumbIndexRange visible {};        // items in the viewport
};

// Which strip viewport the materialized range follows.
enum class ThumbViewportBasis
{
    // The one the next layout pass settles on (a list rebuild, before its
    // pending centering or anchor adjustment is applied).
    Expected,
    // The scroll offset the frame being rendered presents.
    Presented,
};

// The thumbnail strip's geometry and the decisions made from it, without UI
// objects: the layout of the model's items, the scroll adjustment the next
// layout pass makes (ThumbStripScroll), whether that pass must re-arrange the
// strip, and the item ranges for a viewport. The caller reads the viewport
// from the scroll view (none before the first layout pass) and applies the
// results. Every query needs the layout to match the model (Ready): between a
// model commit and the layout rebuild the indices would refer to different
// items.
class ThumbStripViewport
{
public:
    const ThumbStripLayout& Layout() const { return m_layout; }
    bool Ready(const ThumbItemModel& model) const;

    // New extents for every item (a rebuild or a row-height change).
    void Rebuild(std::vector<float> extents, float spacing, float padding);
    // Item `index`'s extent changed (a decoded aspect): the anchor is captured
    // first, so the visible items stay put. Returns false (and changes
    // nothing) when the layout does not match the model.
    bool SetExtent(const ThumbItemModel& model, size_t index, float extent,
        const std::optional<ThumbViewportSpan>& viewport, size_t selected);
    // Remembers the anchor before extents or the model change.
    void CaptureAnchor(const ThumbItemModel& model, const std::optional<ThumbViewportSpan>& viewport, size_t selected);
    void RequestCenter() { m_scroll.RequestCenter(); }

    bool ArrangePending() const { return m_arrangePending; }
    // True while the next layout pass has a re-arrange or a scroll adjustment to make.
    bool SettlePending() const { return m_arrangePending || m_scroll.Pending(); }
    // After a layout pass: the strip is arranged; the scroll adjustment to
    // apply (None without a viewport or before the layout matches the model).
    ThumbStripScroll::Adjustment TakeAfterArrange(const ThumbItemModel& model, const std::optional<ThumbViewportSpan>& viewport);

    // The items that have tiles: the viewport's items plus a prefetch margin
    // of at most ThumbStripLayout::kMaxPrefetchItems on each side, so
    // far-offscreen items have neither a tile nor a decode request. Without a
    // viewport: the selection and the margin around it. Empty until Ready.
    ThumbRanges Ranges(const ThumbItemModel& model, const std::optional<ThumbViewportSpan>& viewport,
        size_t selected, ThumbViewportBasis basis) const;
    // The items overlapping the viewport, if Ready and there is one.
    std::optional<ThumbIndexRange> VisibleRange(const ThumbItemModel& model,
        const std::optional<ThumbViewportSpan>& viewport) const;
    // The items fully inside the viewport (at least 1), if Ready and there is one.
    std::optional<size_t> FullyVisibleCount(const ThumbItemModel& model,
        const std::optional<ThumbViewportSpan>& viewport) const;

private:
    ThumbStripLayout m_layout {};
    ThumbStripScroll m_scroll {};
    bool m_arrangePending { false };
};
