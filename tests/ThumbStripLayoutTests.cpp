#include "TestHarness.h"

#include "ImageBrowserThumbLayout.h"

#include <cmath>
#include <vector>

// Thumbnail-strip geometry from item extents: offsets, the item range for a
// viewport, width estimation, and the anchor kept when widths change.

namespace
{
    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }

    // Padding 4, spacing 4; extents 100 (nav), then images 100, 150, 50, 100.
    ThumbStripLayout Sample()
    {
        ThumbStripLayout layout;
        layout.Reset({ 100.0f, 100.0f, 150.0f, 50.0f, 100.0f }, 4.0f, 4.0f);
        return layout;
    }
}

FIC2_TEST(ThumbStripLayout_OffsetsAndContentWidth)
{
    const ThumbStripLayout layout = Sample();
    FIC2_REQUIRE(layout.Count() == 5);
    FIC2_CHECK(Near(layout.Start(0), 4.0f));
    FIC2_CHECK(Near(layout.End(0), 104.0f));
    FIC2_CHECK(Near(layout.Start(1), 108.0f));
    FIC2_CHECK(Near(layout.Start(2), 212.0f));
    FIC2_CHECK(Near(layout.Start(3), 366.0f));
    FIC2_CHECK(Near(layout.Start(4), 420.0f));
    FIC2_CHECK(Near(layout.ContentWidth(), 524.0f));   // 4 + 500 + 4*4 + 4

    ThumbStripLayout empty;
    empty.Reset({}, 4.0f, 4.0f);
    FIC2_CHECK(Near(empty.ContentWidth(), 8.0f));
    FIC2_CHECK(empty.Range(0.0f, 100.0f).first == empty.Range(0.0f, 100.0f).second);
}

FIC2_TEST(ThumbStripLayout_ChangedExtentMovesOnlyLaterItems)
{
    ThumbStripLayout layout = Sample();
    (void)layout.Start(4);   // offsets computed
    layout.SetExtent(2, 75.0f);   // a decoded aspect arrives for item 2
    FIC2_CHECK(Near(layout.Start(1), 108.0f));
    FIC2_CHECK(Near(layout.Start(2), 212.0f));
    FIC2_CHECK(Near(layout.Start(3), 291.0f));
    FIC2_CHECK(Near(layout.Start(4), 345.0f));
    FIC2_CHECK(Near(layout.ContentWidth(), 449.0f));

    // Two changes before the next read rebuild from the earliest one.
    layout.SetExtent(4, 10.0f);
    layout.SetExtent(0, 50.0f);
    FIC2_CHECK(Near(layout.Start(1), 58.0f));
    FIC2_CHECK(Near(layout.End(4), 305.0f));
}

FIC2_TEST(ThumbStripLayout_RangeCoversPartlyVisibleItems)
{
    const ThumbStripLayout layout = Sample();
    // Viewport [150, 400): item 1 [108,208), 2 [212,362), 3 [366,416).
    auto range = layout.Range(150.0f, 400.0f);
    FIC2_CHECK(range.first == 1 && range.second == 4);
    // Edges: an item ending exactly at `left` or starting at `right` is outside.
    range = layout.Range(104.0f, 212.0f);
    FIC2_CHECK(range.first == 1 && range.second == 2);
    // A viewport in a gap between items still yields an empty or tight range.
    range = layout.Range(208.5f, 211.5f);
    FIC2_CHECK(range.first == range.second);
    // Whole strip and beyond.
    range = layout.Range(-100.0f, 10000.0f);
    FIC2_CHECK(range.first == 0 && range.second == 5);
    range = layout.Range(600.0f, 700.0f);
    FIC2_CHECK(range.first == 5 && range.second == 5);
}

FIC2_TEST(ThumbStripLayout_ImageWidthEstimate)
{
    // Unknown aspect: square at the row height; known: height * aspect.
    FIC2_CHECK(Near(ThumbStripLayout::ImageExtent(128.0f, 0.0f, 0.0f), 128.0f));
    FIC2_CHECK(Near(ThumbStripLayout::ImageExtent(128.0f, 1.5f, 0.0f), 192.0f));
    FIC2_CHECK(Near(ThumbStripLayout::ImageExtent(100.0f, 0.5f, 2.0f), 54.0f));
}

FIC2_TEST(ThumbStripLayout_AnchorKeepsTheVisibleItemInPlace)
{
    ThumbStripLayout layout = Sample();
    const float viewportWidth = 200.0f;

    // Selection (item 3) visible: it is the anchor.
    float scroll = 250.0f;   // viewport [250, 450)
    ThumbStripLayout::Anchor anchor = layout.AnchorFor(scroll, viewportWidth, 3);
    FIC2_REQUIRE(anchor.valid);
    FIC2_CHECK(anchor.index == 3);
    FIC2_CHECK(Near(anchor.offset, 366.0f - 250.0f));

    // Item 1 (before the viewport) and item 2 (partly visible) change width:
    // the anchor's screen position is kept.
    layout.SetExtent(1, 180.0f);
    layout.SetExtent(2, 60.0f);
    scroll = layout.ScrollKeeping(anchor);
    FIC2_CHECK(Near(layout.Start(3) - scroll, anchor.offset));

    // Selection scrolled away: the first visible item anchors instead.
    layout = Sample();
    anchor = layout.AnchorFor(0.0f, viewportWidth, 4);
    FIC2_REQUIRE(anchor.valid);
    FIC2_CHECK(anchor.index == 0);
    FIC2_CHECK(Near(anchor.offset, 4.0f));
    layout.SetExtent(0, 40.0f);   // the anchor itself shrinks: its start stays
    FIC2_CHECK(Near(layout.ScrollKeeping(anchor), 0.0f));

    // No visible item: no anchor.
    FIC2_CHECK(!layout.AnchorFor(5000.0f, viewportWidth, 0).valid);
    FIC2_CHECK(Near(layout.ScrollKeeping({}), 0.0f));
}
