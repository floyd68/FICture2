#include "TestHarness.h"

#include "ImageBrowserThumbStripSizing.h"

// The strip's sizing state without a window: the tile side and its apply
// time, the size-upgrade deferral and the strip-height broadcast held during
// a splitter drag.

FIC2_TEST(ThumbStripSizing_Side)
{
    ThumbStripSizing s;
    FIC2_CHECK(s.Side() == ThumbStripSizing::kDefaultSide);
    FIC2_CHECK(s.LastApplyMs() == 0);

    s.SetSide(96.0f);
    s.MarkApplied(5000);
    FIC2_CHECK(s.Side() == 96.0f);
    FIC2_CHECK(s.LastApplyMs() == 5000);

    // A reset restores the side only; the throttle keeps its last apply time.
    s.ResetSide();
    FIC2_CHECK(s.Side() == ThumbStripSizing::kDefaultSide);
    FIC2_CHECK(s.LastApplyMs() == 5000);
}

FIC2_TEST(ThumbStripSizing_UpgradeDeferralReportsChanges)
{
    ThumbStripSizing s;
    FIC2_CHECK(!s.UpgradesDeferred());
    FIC2_CHECK(!s.SetUpgradesDeferred(false));   // unchanged
    FIC2_CHECK(s.SetUpgradesDeferred(true));
    FIC2_CHECK(s.UpgradesDeferred());
    FIC2_CHECK(!s.SetUpgradesDeferred(true));
    FIC2_CHECK(s.SetUpgradesDeferred(false));
    FIC2_CHECK(!s.UpgradesDeferred());
}

FIC2_TEST(ThumbStripSizing_BroadcastWaitsForTheDragToEnd)
{
    ThumbStripSizing s;
    FIC2_CHECK(!s.BroadcastPending());
    FIC2_CHECK(!s.TakeBroadcast(false).has_value());

    s.DeferBroadcast(120.0f);
    s.DeferBroadcast(150.0f);                  // the latest height wins
    FIC2_CHECK(s.BroadcastPending());
    FIC2_CHECK(!s.TakeBroadcast(true).has_value());   // still dragging
    FIC2_CHECK(s.BroadcastPending());

    const auto height = s.TakeBroadcast(false);
    FIC2_REQUIRE(height.has_value());
    FIC2_CHECK(*height == 150.0f);
    FIC2_CHECK(!s.BroadcastPending());
    FIC2_CHECK(!s.TakeBroadcast(false).has_value());  // taken once
}
