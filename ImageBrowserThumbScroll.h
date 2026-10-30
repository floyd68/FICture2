#pragma once

#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbModel.h"

#include <cstddef>
#include <string>

// The scroll adjustment the thumbnail strip makes after its next layout pass,
// without UI objects. Viewports are [left, left + width) in panel coordinates.
//  - A pending centering (new selection, rebuilt list, resized strip) centers
//    the selection; it wins over the anchor.
//  - Otherwise the anchor item captured before extents changed (the selection
//    if visible, else the first visible item) is put back at its viewport
//    offset. It is found by lookup key, so a rebuild that moves its index
//    keeps it, and it is dropped when its item is gone.
class ThumbStripScroll
{
public:
    void RequestCenter() { m_centerPending = true; }
    // True while the next layout pass has an adjustment to apply.
    bool Pending() const { return m_centerPending || m_anchor.valid; }

    // Remembers the anchor before extents change: once per layout pass, and
    // not while a centering is pending.
    void CaptureAnchor(const ThumbItemModel& model, const ThumbStripLayout& layout,
        float left, float width, size_t selected);

    // The viewport's left edge after the adjustment, clamped to the content.
    float ExpectedLeft(const ThumbItemModel& model, const ThumbStripLayout& layout,
        float left, float width, size_t selected) const;

    struct Adjustment
    {
        enum class Kind { None, CenterSelection, Shift };
        Kind kind { Kind::None };
        float shift { 0.0f };   // Kind::Shift: scroll delta
    };
    // Consumes the adjustment for the viewport the layout pass settled on. A
    // viewport width different from the last one requests centering first.
    Adjustment Take(const ThumbItemModel& model, const ThumbStripLayout& layout, float left, float width);

private:
    struct Anchor
    {
        bool valid { false };
        std::wstring key {};   // ThumbItemModel lookup key of the anchor item
        float offset { 0.0f }; // its start relative to the viewport's left edge
    };

    Anchor m_anchor {};
    bool m_centerPending { false };
    float m_lastViewportWidth { 0.0f };
};
