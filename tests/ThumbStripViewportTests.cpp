#include "TestHarness.h"

#include "ImageBrowserThumbViewport.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// The strip's geometry decisions for a viewport: readiness, the materialized
// and visible ranges, the paging step, and the re-arrange / scroll adjustment
// around a layout pass.

namespace
{
    using Kind = ThumbStripScroll::Adjustment::Kind;

    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\Pictures\\Trip"));
    constexpr float kSpacing = 4.0f;
    constexpr float kPadding = 4.0f;

    bool IsImage(const Floar::VirtualPath&)
    {
        return true;
    }

    // `count` images (no nav items), named so that name order is index order.
    ThumbItemModel Model(size_t count)
    {
        std::vector<Floar::VirtualFileEntry> files;
        for (size_t i = 0; i < count; ++i)
        {
            wchar_t name[32] {};
            swprintf_s(name, L"%03zu.png", i);
            files.emplace_back(Floar::VirtualPath(kFolder.hostPath / name), false, 0, 0);
        }
        ThumbItemModel model;
        model.Reset(kFolder, false, &IsImage);
        (void)model.Merge(files);
        return model;
    }

    // Extents 100: item i spans [4 + 104 * i, 104 + 104 * i).
    ThumbStripViewport Viewport(size_t count)
    {
        ThumbStripViewport viewport;
        viewport.Rebuild(std::vector<float>(count, 100.0f), kSpacing, kPadding);
        return viewport;
    }

    std::optional<ThumbViewportSpan> Span(float left, float width)
    {
        return ThumbViewportSpan { left, width };
    }

    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }
}

FIC2_TEST(ThumbStripViewport_QueriesWaitForTheLayoutToMatchTheModel)
{
    const ThumbItemModel empty = Model(0);
    const ThumbItemModel model = Model(5);
    ThumbStripViewport viewport;
    FIC2_CHECK(!viewport.Ready(empty));
    FIC2_CHECK(!viewport.Ready(model));

    viewport.Rebuild(std::vector<float>(4, 100.0f), kSpacing, kPadding);   // a stale layout
    FIC2_CHECK(!viewport.Ready(model));
    FIC2_CHECK(viewport.Ranges(model, Span(0.0f, 200.0f), 0, ThumbViewportBasis::Presented).materialized.Size() == 0);
    FIC2_CHECK(!viewport.VisibleRange(model, Span(0.0f, 200.0f)));
    FIC2_CHECK(!viewport.FullyVisibleCount(model, Span(0.0f, 200.0f)));
    (void)viewport.TakeAfterArrange(model, Span(0.0f, 200.0f));
    FIC2_CHECK(!viewport.SetExtent(model, 1, 150.0f, Span(0.0f, 200.0f), 0));
    FIC2_CHECK(!viewport.ArrangePending());

    viewport.Rebuild(std::vector<float>(5, 100.0f), kSpacing, kPadding);
    FIC2_CHECK(viewport.Ready(model));
    FIC2_CHECK(viewport.ArrangePending());
    FIC2_CHECK(viewport.Layout().Count() == 5);
}

FIC2_TEST(ThumbStripViewport_RangesWithoutAViewportFollowTheSelection)
{
    const ThumbItemModel model = Model(100);
    const ThumbStripViewport viewport = Viewport(100);
    const size_t margin = ThumbStripLayout::kMaxPrefetchItems;

    const ThumbRanges around = viewport.Ranges(model, std::nullopt, 50, ThumbViewportBasis::Expected);
    FIC2_CHECK(around.materialized == (ThumbIndexRange { 50 - margin, 50 + margin + 1 }));
    FIC2_CHECK(around.visible.Size() == 0);

    // No valid selection: around the first item.
    const ThumbRanges first = viewport.Ranges(model, std::nullopt, static_cast<size_t>(-1), ThumbViewportBasis::Expected);
    FIC2_CHECK(first.materialized == (ThumbIndexRange { 0, margin + 1 }));
}

FIC2_TEST(ThumbStripViewport_ExpectedRangesFollowAPendingCentering)
{
    const ThumbItemModel model = Model(100);
    ThumbStripViewport viewport = Viewport(100);
    (void)viewport.TakeAfterArrange(model, Span(0.0f, 200.0f));   // the first pass centers

    // Viewport [0, 200): items 0 and 1; one viewport's worth (2) of margin.
    const ThumbRanges presented = viewport.Ranges(model, Span(0.0f, 200.0f), 50, ThumbViewportBasis::Presented);
    FIC2_CHECK(presented.visible == (ThumbIndexRange { 0, 2 }));
    FIC2_CHECK(presented.materialized == (ThumbIndexRange { 0, 4 }));

    // Centering item 50 ([5204, 5304)) puts the viewport at [5154, 5354):
    // items 49 to 51, with a margin of 3.
    viewport.RequestCenter();
    const ThumbRanges expected = viewport.Ranges(model, Span(0.0f, 200.0f), 50, ThumbViewportBasis::Expected);
    FIC2_CHECK(expected.visible == (ThumbIndexRange { 49, 52 }));
    FIC2_CHECK(expected.materialized == (ThumbIndexRange { 46, 55 }));
    // The frame being rendered still presents the current offset.
    FIC2_CHECK(viewport.Ranges(model, Span(0.0f, 200.0f), 50, ThumbViewportBasis::Presented).visible == presented.visible);
}

FIC2_TEST(ThumbStripViewport_VisibleRangeAndFullyVisibleCount)
{
    const ThumbItemModel model = Model(10);
    const ThumbStripViewport viewport = Viewport(10);
    FIC2_CHECK(!viewport.VisibleRange(model, std::nullopt));
    FIC2_CHECK(!viewport.FullyVisibleCount(model, std::nullopt));

    // [50, 350): items 0 and 3 are partly visible, 1 and 2 fully.
    FIC2_CHECK(viewport.VisibleRange(model, Span(50.0f, 300.0f)) == (ThumbIndexRange { 0, 4 }));
    FIC2_CHECK(viewport.FullyVisibleCount(model, Span(50.0f, 300.0f)) == 2u);
    // Half a pixel of tolerance: [108, 312) holds items 1 and 2 exactly.
    FIC2_CHECK(viewport.FullyVisibleCount(model, Span(108.4f, 203.2f)) == 2u);
    // Narrower than one item: still a step of 1.
    FIC2_CHECK(viewport.FullyVisibleCount(model, Span(10.0f, 50.0f)) == 1u);
}

FIC2_TEST(ThumbStripViewport_ExtentChangeKeepsTheAnchorAcrossTheArrange)
{
    const ThumbItemModel model = Model(10);
    ThumbStripViewport viewport = Viewport(10);
    FIC2_CHECK(viewport.TakeAfterArrange(model, Span(110.0f, 200.0f)).kind == Kind::CenterSelection);
    FIC2_CHECK(!viewport.ArrangePending());
    FIC2_CHECK(!viewport.SettlePending());

    // [110, 310): item 1 is the first visible one (2 px left of the edge).
    // Item 0 widens by 50, so item 1 moves to 158.
    FIC2_CHECK(viewport.SetExtent(model, 0, 150.0f, Span(110.0f, 200.0f), static_cast<size_t>(-1)));
    FIC2_CHECK(Near(viewport.Layout().Start(1), 158.0f));
    FIC2_CHECK(viewport.ArrangePending());
    FIC2_CHECK(viewport.SettlePending());

    // A layout pass without a viewport arranges but keeps the anchor.
    FIC2_CHECK(viewport.TakeAfterArrange(model, std::nullopt).kind == Kind::None);
    FIC2_CHECK(!viewport.ArrangePending());
    FIC2_CHECK(viewport.SettlePending());

    const ThumbStripScroll::Adjustment adjustment = viewport.TakeAfterArrange(model, Span(110.0f, 200.0f));
    FIC2_CHECK(adjustment.kind == Kind::Shift);
    FIC2_CHECK(Near(adjustment.shift, 50.0f));
    FIC2_CHECK(!viewport.SettlePending());
}

FIC2_TEST(ThumbStripViewport_ExtentChangeWithoutAViewportCapturesNothing)
{
    const ThumbItemModel model = Model(10);
    ThumbStripViewport viewport = Viewport(10);
    (void)viewport.TakeAfterArrange(model, Span(110.0f, 200.0f));

    FIC2_CHECK(viewport.SetExtent(model, 0, 150.0f, std::nullopt, 0));
    FIC2_CHECK(viewport.ArrangePending());
    FIC2_CHECK(viewport.TakeAfterArrange(model, Span(110.0f, 200.0f)).kind == Kind::None);
    FIC2_CHECK(!viewport.SettlePending());
}
