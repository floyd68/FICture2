#include "TestHarness.h"

#include "ImageBrowserThumbScroll.h"

#include <cmath>
#include <vector>

// The strip's scroll adjustment after a layout pass: centering the selection,
// or putting the anchor item back at its viewport offset (found by key).

namespace
{
    using Kind = ThumbStripScroll::Adjustment::Kind;

    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\Pictures\\Trip"));
    constexpr float kWidth = 200.0f;

    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }

    Floar::VirtualFileEntry File(const wchar_t* name)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(kFolder.hostPath / name), false, 0, 0);
    }

    bool IsImage(const Floar::VirtualPath&)
    {
        return true;
    }

    // Images only (no nav items), in name order.
    ThumbItemModel Model(const std::vector<Floar::VirtualFileEntry>& files)
    {
        ThumbItemModel model;
        model.Reset(kFolder, false, &IsImage);
        (void)model.Merge(files);
        return model;
    }

    ThumbItemModel FiveImages()
    {
        return Model({ File(L"a.png"), File(L"b.png"), File(L"c.png"), File(L"d.png"), File(L"e.png") });
    }

    // Extents 100, padding 4, spacing 4: item i starts at 4 + 104 * i.
    ThumbStripLayout Layout(size_t count)
    {
        ThumbStripLayout layout;
        layout.Reset(std::vector<float>(count, 100.0f), 4.0f, 4.0f);
        return layout;
    }

    std::wstring Key(const wchar_t* name)
    {
        return ThumbItemModel::LookupKeyFor(File(name).path);
    }

    // A scroll state past its first layout pass (which centers: the viewport
    // width was unknown before).
    ThumbStripScroll Settled(const ThumbItemModel& model, const ThumbStripLayout& layout)
    {
        ThumbStripScroll scroll;
        (void)scroll.Take(model, layout, 0.0f, kWidth);
        return scroll;
    }
}

FIC2_TEST(ThumbStripScroll_FirstPassAndResizeCenter)
{
    const ThumbItemModel model = FiveImages();
    const ThumbStripLayout layout = Layout(5);
    ThumbStripScroll scroll;
    FIC2_CHECK(scroll.Take(model, layout, 0.0f, kWidth).kind == Kind::CenterSelection);
    FIC2_CHECK(scroll.Take(model, layout, 0.0f, kWidth).kind == Kind::None);

    // Less than half a pixel is not a resize.
    FIC2_CHECK(scroll.Take(model, layout, 0.0f, kWidth + 0.4f).kind == Kind::None);

    // A resize centers and drops a captured anchor.
    scroll.CaptureAnchor(model, layout, 110.0f, kWidth, 1);
    FIC2_CHECK(scroll.Pending());
    FIC2_CHECK(scroll.Take(model, layout, 110.0f, 300.0f).kind == Kind::CenterSelection);
    FIC2_CHECK(!scroll.Pending());
    FIC2_CHECK(scroll.Take(model, layout, 110.0f, 300.0f).kind == Kind::None);
}

FIC2_TEST(ThumbStripScroll_CenteringWinsOverTheAnchor)
{
    const ThumbItemModel model = FiveImages();
    const ThumbStripLayout layout = Layout(5);
    ThumbStripScroll scroll = Settled(model, layout);

    scroll.CaptureAnchor(model, layout, 110.0f, kWidth, 1);
    scroll.RequestCenter();
    // Item 3 spans [316, 416): centered at 266.
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 110.0f, kWidth, 3), 266.0f));
    FIC2_CHECK(scroll.Take(model, layout, 110.0f, kWidth).kind == Kind::CenterSelection);
    FIC2_CHECK(!scroll.Pending());

    // No anchor is captured while a centering is pending.
    scroll.RequestCenter();
    scroll.CaptureAnchor(model, layout, 110.0f, kWidth, 1);
    FIC2_CHECK(scroll.Take(model, layout, 110.0f, kWidth).kind == Kind::CenterSelection);
    FIC2_CHECK(scroll.Take(model, layout, 110.0f, kWidth).kind == Kind::None);

    // Without a valid selection the expected viewport follows the anchor
    // (b.png, 2 px left of the viewport's edge).
    scroll.CaptureAnchor(model, layout, 110.0f, kWidth, 99);
    scroll.RequestCenter();
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 120.0f, kWidth, 99), 110.0f));
}

FIC2_TEST(ThumbStripScroll_AnchorKeptByKeyAcrossARebuild)
{
    const ThumbItemModel before = FiveImages();
    const ThumbStripLayout beforeLayout = Layout(5);
    ThumbStripScroll scroll = Settled(before, beforeLayout);

    // Viewport [110, 310): item 0 ends at 104, so the first visible item is
    // b.png (index 1, start 108), 2 px left of the viewport's edge.
    scroll.CaptureAnchor(before, beforeLayout, 110.0f, kWidth, 99);
    // Captured once per layout pass: a second capture keeps the first.
    scroll.CaptureAnchor(before, beforeLayout, 300.0f, kWidth, 99);

    // A rebuild adds an item before it: b.png moves to index 2 (start 212).
    const ThumbItemModel after = Model({ File(L"0.png"), File(L"a.png"), File(L"b.png"),
        File(L"c.png"), File(L"d.png"), File(L"e.png") });
    const ThumbStripLayout afterLayout = Layout(6);
    FIC2_REQUIRE(after.IndexOf(Key(L"b.png")) == 2u);
    FIC2_CHECK(Near(scroll.ExpectedLeft(after, afterLayout, 110.0f, kWidth, 99), 214.0f));

    const ThumbStripScroll::Adjustment adjustment = scroll.Take(after, afterLayout, 110.0f, kWidth);
    FIC2_CHECK(adjustment.kind == Kind::Shift);
    FIC2_CHECK(Near(adjustment.shift, 104.0f));
    FIC2_CHECK(!scroll.Pending());
    FIC2_CHECK(scroll.Take(after, afterLayout, 214.0f, kWidth).kind == Kind::None);
}

FIC2_TEST(ThumbStripScroll_AnchorPrefersAVisibleSelection)
{
    const ThumbItemModel model = FiveImages();
    ThumbStripLayout layout = Layout(5);
    ThumbStripScroll scroll = Settled(model, layout);

    // The selection c.png (index 2, start 212) is visible: it is the anchor.
    scroll.CaptureAnchor(model, layout, 110.0f, kWidth, 2);
    // b.png widens by 50: c.png moves to 262 and keeps its offset 102.
    layout.SetExtent(1, 150.0f);
    const ThumbStripScroll::Adjustment adjustment = scroll.Take(model, layout, 110.0f, kWidth);
    FIC2_CHECK(adjustment.kind == Kind::Shift);
    FIC2_CHECK(Near(adjustment.shift, 50.0f));
}

FIC2_TEST(ThumbStripScroll_AnchorDroppedWhenItsItemIsGone)
{
    const ThumbItemModel before = FiveImages();
    const ThumbStripLayout beforeLayout = Layout(5);
    ThumbStripScroll scroll = Settled(before, beforeLayout);
    scroll.CaptureAnchor(before, beforeLayout, 110.0f, kWidth, 99);   // b.png

    const ThumbItemModel after = Model({ File(L"a.png"), File(L"c.png"), File(L"d.png") });
    const ThumbStripLayout afterLayout = Layout(3);   // content width 316
    FIC2_CHECK(Near(scroll.ExpectedLeft(after, afterLayout, 110.0f, kWidth, 99), 110.0f));
    FIC2_CHECK(scroll.Take(after, afterLayout, 110.0f, kWidth).kind == Kind::None);
    FIC2_CHECK(!scroll.Pending());
}

FIC2_TEST(ThumbStripScroll_ExpectedLeftIsClampedToTheContent)
{
    const ThumbItemModel model = FiveImages();
    const ThumbStripLayout layout = Layout(5);   // content width 524
    ThumbStripScroll scroll = Settled(model, layout);
    scroll.RequestCenter();
    // Item 0 would center at -46; item 4 at 370, past the last left edge 324.
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 100.0f, kWidth, 0), 0.0f));
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 100.0f, kWidth, 4), 324.0f));
    // Nothing pending: the current viewport, clamped.
    (void)scroll.Take(model, layout, 100.0f, kWidth);
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 100.0f, kWidth, 0), 100.0f));
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, layout, 400.0f, kWidth, 0), 324.0f));
}

FIC2_TEST(ThumbStripScroll_WaitsForTheLayoutToMatchTheModel)
{
    // Between a model commit and the layout rebuild nothing is captured or
    // consumed: the layout's indices would refer to different items.
    const ThumbItemModel model = FiveImages();
    const ThumbStripLayout stale = Layout(4);
    ThumbStripScroll scroll = Settled(model, Layout(5));
    scroll.RequestCenter();
    FIC2_CHECK(scroll.Take(model, stale, 0.0f, kWidth).kind == Kind::None);
    FIC2_CHECK(scroll.Pending());
    FIC2_CHECK(Near(scroll.ExpectedLeft(model, stale, 50.0f, kWidth, 1), 50.0f));
    FIC2_CHECK(scroll.Take(model, Layout(5), 0.0f, kWidth).kind == Kind::CenterSelection);

    scroll.CaptureAnchor(model, stale, 110.0f, kWidth, 1);
    FIC2_CHECK(!scroll.Pending());
}
