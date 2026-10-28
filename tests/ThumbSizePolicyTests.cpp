#include "TestHarness.h"

#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserThumbSizePolicy.h"

#include <cmath>
#include <cstdint>
#include <vector>

// The thumbnail decode-size policy on its own (#40): buckets, the request box,
// sufficiency against the actual decoded size, the choice among cached sizes,
// and the hysteresis and request rules that keep a splitter drag from
// decoding again and again. A tiny simulation stands in for the strip: an
// item whose decodes complete at once with ImageCore's fit (min scale,
// rounded, never upscaled).

namespace
{
    using namespace ThumbSizePolicy;

    // ImageCore's thumbnail fit (ImageDecodeDispatcher): the source scaled by
    // min(box.w / w, box.h / h), rounded, when either side exceeds the box;
    // never upscaled.
    Decoded Fit(uint32_t sourceWidth, uint32_t sourceHeight, const ImageCore::Size& box)
    {
        uint32_t width = sourceWidth;
        uint32_t height = sourceHeight;
        if (static_cast<float>(sourceWidth) > box.w || static_cast<float>(sourceHeight) > box.h)
        {
            const float scale = (std::min)(box.w / static_cast<float>(sourceWidth), box.h / static_cast<float>(sourceHeight));
            width = static_cast<uint32_t>(std::round(sourceWidth * scale));
            height = static_cast<uint32_t>(std::round(sourceHeight * scale));
        }
        return { width, height, ThumbPayloadCache::IsSourceLimited(width, height, box) };
    }

    // One strip item: what it shows, its binding's upgrade tracker, and every
    // decode it requested. Decodes complete at once unless `holdDecodes`.
    struct Item
    {
        uint32_t sourceWidth { 0 };
        uint32_t sourceHeight { 0 };
        float knownAspect { 0.0f };
        Decoded shown {};
        UpgradeTracker tracker {};
        std::vector<ImageCore::Size> requests {};
        bool holdDecodes { false };
        bool inFlight { false };
        ImageCore::Size inFlightBox {};

        // One frame at `rowHeight`: evaluates the policy and issues its request.
        void Frame(float rowHeight, bool defer = false)
        {
            Input input {};
            input.rowHeight = rowHeight;
            input.knownAspect = knownAspect;
            input.shown = shown;
            input.requestInFlight = inFlight;
            input.deferUpgrades = defer;
            const Decision decision = Evaluate(input, tracker);
            if (decision.action == Action::None)
            {
                return;
            }
            if (decision.action == Action::Upgrade)
            {
                tracker.MarkRequested(decision.bucket);
            }
            requests.push_back(decision.box);
            inFlight = true;
            inFlightBox = decision.box;
            if (!holdDecodes)
            {
                Land();
            }
        }

        void Land()
        {
            if (!inFlight)
            {
                return;
            }
            inFlight = false;
            const Decoded decoded = Fit(sourceWidth, sourceHeight, inFlightBox);
            // A smaller result never replaces a larger one (ImageBrowserThumbImage).
            if (shown.Empty() || decoded.height >= shown.height)
            {
                shown = decoded;
            }
        }

        // Displayed size at `rowHeight`, as the strip lays it out from the
        // shown payload's aspect (ThumbStripLayout::ImageExtent).
        bool ReachesDisplayedSize(float rowHeight) const
        {
            if (shown.sourceLimited)
            {
                return true;
            }
            const float displayedWidth = rowHeight * shown.Aspect();
            return static_cast<float>(shown.height) + kRoundingTolerance >= rowHeight &&
                static_cast<float>(shown.width) + kRoundingTolerance >= std::floor(displayedWidth);
        }
    };
}

FIC2_TEST(ThumbSizePolicy_BucketsCoverEveryRowHeightFromAbove)
{
    FIC2_CHECK(BucketFor(0.0f) == 0.0f);
    FIC2_CHECK(BucketFor(-5.0f) == 0.0f);
    FIC2_CHECK(BucketFor(1.0f) == 32.0f);
    FIC2_CHECK(BucketFor(32.0f) == 32.0f);
    FIC2_CHECK(BucketFor(33.0f) == 48.0f);
    FIC2_CHECK(BucketFor(128.0f) == 128.0f);
    FIC2_CHECK(BucketFor(129.0f) == 192.0f);
    FIC2_CHECK(BucketFor(256.0f) == 256.0f);
    FIC2_CHECK(BucketFor(257.0f) == 320.0f);
    FIC2_CHECK(BucketFor(400.0f) == 448.0f);

    // Every row height from the strip's minimum to past its maximum: the
    // bucket reaches it, is at most 1.5x it, and never shrinks as it grows;
    // the bucket index orders buckets.
    float previous = 0.0f;
    for (int row = 32; row <= 600; ++row)
    {
        const float bucket = BucketFor(static_cast<float>(row));
        FIC2_CHECK(bucket >= static_cast<float>(row));
        FIC2_CHECK(bucket <= static_cast<float>(row) * 1.5f);
        FIC2_CHECK(bucket >= previous);
        if (bucket > previous && previous > 0.0f)
        {
            FIC2_CHECK(BucketIndex(bucket) == BucketIndex(previous) + 1);
        }
        previous = bucket;
    }
}

FIC2_TEST(ThumbSizePolicy_RequestBoxIsSquareUntilTheAspectIsKnown)
{
    const ImageCore::Size square = RequestBox(128.0f, 0.0f);
    FIC2_CHECK(square.w == 128.0f && square.h == 128.0f);
    const ImageCore::Size none = RequestBox(0.0f, 1.5f);
    FIC2_CHECK(none.w == 0.0f && none.h == 0.0f);

    // With the aspect measured on a square-box decode (rounded to whole
    // pixels), the box's height is the side that binds, so the decode has
    // the bucket's height and the source's aspect: the displayed size.
    struct Source
    {
        uint32_t width;
        uint32_t height;
    };
    const Source sources[] {
        { 1920, 1080 }, { 3000, 1000 }, { 4032, 3024 }, { 4032, 1920 }, { 1080, 1920 }, { 6000, 600 }, { 9000, 1000 },
        { 997, 991 },
    };
    for (const Source& source : sources)
    {
        for (const float bucket : kBuckets)
        {
            const Decoded first = Fit(source.width, source.height, RequestBox(bucket, 0.0f));
            const ImageCore::Size box = RequestBox(bucket, first.AspectUpperBound());
            FIC2_CHECK(box.h == bucket);
            const Decoded decoded = Fit(source.width, source.height, box);
            FIC2_CHECK(decoded.height == static_cast<uint32_t>(bucket));
            const float displayedWidth = bucket * static_cast<float>(source.width) / static_cast<float>(source.height);
            FIC2_CHECK(std::fabs(static_cast<float>(decoded.width) - displayedWidth) <= 1.0f);
            FIC2_CHECK(!decoded.sourceLimited);
            FIC2_CHECK(Satisfies(decoded, bucket));
        }
    }
}

FIC2_TEST(ThumbSizePolicy_SufficiencyComparesTheDecodedSizeWithTheBucket)
{
    // A 16:9 image decoded in the square box: shown 228 x 128, decoded 72 tall.
    FIC2_CHECK(!Satisfies({ 128, 72, false }, 128.0f));
    FIC2_CHECK(Satisfies({ 228, 128, false }, 128.0f));
    FIC2_CHECK(Satisfies({ 227, 127, false }, 128.0f));    // rounding
    FIC2_CHECK(!Satisfies({ 226, 126, false }, 128.0f));
    FIC2_CHECK(Satisfies({ 228, 128, false }, 96.0f));     // a larger decode serves a smaller bucket
    FIC2_CHECK(!Satisfies({ 228, 128, false }, 192.0f));   // ... never a larger one
    FIC2_CHECK(!Satisfies({}, 32.0f));

    // A source with no more pixels satisfies every bucket.
    FIC2_CHECK(Satisfies({ 40, 30, true }, 256.0f));
    FIC2_CHECK(Satisfies({ 40, 30, true }, 1024.0f));
    // So does a payload as tall as the item's recorded source-limited height.
    FIC2_CHECK(Satisfies({ 40, 30, false }, 256.0f, 30));
    FIC2_CHECK(!Satisfies({ 20, 15, false }, 256.0f, 30));
}

FIC2_TEST(ThumbSizePolicy_UndersizedCacheEntriesNeverSatisfyALargerBucket)
{
    // Only undersized entries: the largest is shown, and it does not satisfy.
    const std::vector<Decoded> undersized { { 64, 64, false }, { 128, 128, false }, { 96, 96, false } };
    CacheChoice choice = ChooseCached(undersized, 192.0f);
    FIC2_REQUIRE(choice.Found());
    FIC2_CHECK(choice.index == 1);
    FIC2_CHECK(!choice.satisfies);

    // The smallest that satisfies, a larger one included.
    const std::vector<Decoded> sizes { { 256, 256, false }, { 64, 64, false }, { 128, 128, false } };
    choice = ChooseCached(sizes, 128.0f);
    FIC2_CHECK(choice.index == 2 && choice.satisfies);
    choice = ChooseCached(sizes, 96.0f);
    FIC2_CHECK(choice.index == 2 && choice.satisfies);
    choice = ChooseCached(sizes, 192.0f);
    FIC2_CHECK(choice.index == 0 && choice.satisfies);
    choice = ChooseCached({ { 512, 256, false } }, 64.0f);
    FIC2_CHECK(choice.index == 0 && choice.satisfies);

    // A source-limited entry satisfies any larger bucket.
    choice = ChooseCached({ { 40, 30, true } }, 256.0f);
    FIC2_CHECK(choice.index == 0 && choice.satisfies);
    FIC2_CHECK(!ChooseCached({}, 128.0f).Found());
}

// The drag rules: a smaller row height never asks for a smaller decode, a
// reversal across a bucket boundary costs at most one decode, and each bucket
// is requested at most once per binding.
FIC2_TEST(ThumbSizePolicy_ResizeReversalsDecodeAtMostOncePerBucket)
{
    // Square source, decoded at 128 first.
    Item item { 2000, 2000 };
    item.Frame(128.0f);
    FIC2_REQUIRE(item.requests.size() == 1);

    // Across the 128/192 boundary and back, fifty times: one upgrade (192),
    // which then serves both sides.
    for (int i = 0; i < 50; ++i)
    {
        item.Frame(129.0f);
        item.Frame(127.0f);
        item.Frame(100.0f);
    }
    FIC2_CHECK(item.requests.size() == 2);
    FIC2_CHECK(item.shown.height == 192);

    // Down to the minimum and back up: nothing (192 is held).
    for (float row = 192.0f; row >= 32.0f; row -= 7.0f)
    {
        item.Frame(row);
    }
    for (float row = 32.0f; row <= 192.0f; row += 5.0f)
    {
        item.Frame(row);
    }
    FIC2_CHECK(item.requests.size() == 2);

    // A full drag 32 -> 256 -> 32 -> 256 decoding at once: at most one decode
    // per bucket crossed upward, and none on the way down or on the second pass.
    Item fresh { 2000, 2000 };
    fresh.Frame(32.0f);
    for (int pass = 0; pass < 2; ++pass)
    {
        for (float row = 32.0f; row <= 256.0f; row += 3.0f)
        {
            fresh.Frame(row);
        }
        for (float row = 256.0f; row >= 32.0f; row -= 3.0f)
        {
            fresh.Frame(row);
        }
    }
    FIC2_CHECK(fresh.requests.size() == kBucketCount);   // 32 first, then one per larger bucket
    FIC2_CHECK(fresh.tracker.Count() == kBucketCount - 1);
}

// While the row height moves (splitter drag), upgrades wait; once it settles,
// one upgrade serves the final row height. A first decode is never deferred.
FIC2_TEST(ThumbSizePolicy_DeferredUpgradesWaitForTheRowHeightToSettle)
{
    Item item { 2000, 2000 };
    item.Frame(64.0f, true);   // nothing to show: decoded even while deferred
    FIC2_REQUIRE(item.requests.size() == 1);

    for (int i = 0; i < 20; ++i)
    {
        item.Frame(256.0f, true);
        item.Frame(48.0f, true);
        item.Frame(200.0f, true);
    }
    FIC2_CHECK(item.requests.size() == 1);

    item.Frame(200.0f);   // settled
    FIC2_CHECK(item.requests.size() == 2);
    FIC2_CHECK(item.requests.back().h == 256.0f);
    item.Frame(200.0f);
    item.Frame(256.0f);
    FIC2_CHECK(item.requests.size() == 2);
}

// One request in flight per binding: a growing row height while an upgrade
// is in flight does not stack requests, and the next bucket is evaluated
// once the first lands.
FIC2_TEST(ThumbSizePolicy_OneRequestInFlightPerBinding)
{
    Item item { 2000, 2000 };
    item.holdDecodes = true;
    item.Frame(64.0f);
    for (float row = 64.0f; row <= 256.0f; row += 4.0f)
    {
        item.Frame(row);
    }
    FIC2_CHECK(item.requests.size() == 1);   // the first decode is still in flight
    item.Land();
    item.Frame(256.0f);
    FIC2_CHECK(item.requests.size() == 2);
    item.Frame(64.0f);
    item.Frame(256.0f);
    FIC2_CHECK(item.requests.size() == 2);   // in flight
    item.Land();
    item.Frame(256.0f);
    FIC2_CHECK(item.requests.size() == 2);
    FIC2_CHECK(item.shown.height == 256);
}

// A result that is neither large enough nor source-limited (an embedded
// thumbnail that ignores the target, say) is not requested again for the
// same bucket: no loop.
FIC2_TEST(ThumbSizePolicy_AnUnexpectedlySmallResultCannotLoop)
{
    UpgradeTracker tracker {};
    Input input {};
    input.rowHeight = 128.0f;
    input.shown = { 160, 90, false };
    Decision decision = Evaluate(input, tracker);
    FIC2_REQUIRE(decision.action == Action::Upgrade);
    FIC2_CHECK(decision.bucket == 128.0f);
    FIC2_CHECK(decision.box.h == 128.0f);
    FIC2_CHECK(decision.box.w >= 128.0f * 160.0f / 90.0f);
    tracker.MarkRequested(decision.bucket);

    // The upgrade "completed" with the same small size: withheld from now on.
    for (int i = 0; i < 10; ++i)
    {
        decision = Evaluate(input, tracker);
        FIC2_CHECK(decision.action == Action::None);
        FIC2_CHECK(decision.upgradeWithheld);
    }
    // A larger bucket is a different request (once).
    input.rowHeight = 150.0f;
    decision = Evaluate(input, tracker);
    FIC2_CHECK(decision.action == Action::Upgrade && decision.bucket == 192.0f);
    tracker.MarkRequested(decision.bucket);
    FIC2_CHECK(Evaluate(input, tracker).action == Action::None);
    FIC2_CHECK(tracker.Count() == 2);
    tracker.Reset();
    FIC2_CHECK(tracker.Count() == 0);
}

// A source smaller than every bucket: one decode, source-limited, and no
// upgrade at any larger row height.
FIC2_TEST(ThumbSizePolicy_ASmallSourceDecodesOnceAcrossBuckets)
{
    Item item { 40, 30 };
    item.Frame(64.0f);
    FIC2_REQUIRE(item.shown.sourceLimited);
    for (float row = 64.0f; row <= 400.0f; row += 9.0f)
    {
        item.Frame(row);
    }
    FIC2_CHECK(item.requests.size() == 1);
    FIC2_CHECK(item.shown.width == 40 && item.shown.height == 30);
}

// At a constant row height, a 16:9, a 3:1, a camera-like 4:3 and 2.1:1 and a
// portrait source each reach the displayed size after at most one upgrade,
// and further frames request nothing. A known aspect (from the item model or
// the cache) saves the upgrade.
FIC2_TEST(ThumbSizePolicy_SourcesReachTheDisplayedSizeAfterAtMostOneUpgrade)
{
    struct Source
    {
        uint32_t width;
        uint32_t height;
    };
    const Source sources[] { { 1920, 1080 }, { 3000, 1000 }, { 4032, 3024 }, { 4032, 1920 }, { 1080, 1920 } };
    const float rows[] { 32.0f, 64.0f, 100.0f, 128.0f, 177.0f, 256.0f };
    for (const Source& source : sources)
    {
        for (const float row : rows)
        {
            Item item { source.width, source.height };
            for (int frame = 0; frame < 5; ++frame)
            {
                item.Frame(row);
            }
            FIC2_CHECK(item.requests.size() <= 2);
            FIC2_CHECK(item.tracker.Count() <= 1);
            FIC2_CHECK(item.ReachesDisplayedSize(row));
            // Portrait images fill the square box's height: no upgrade at all.
            if (source.height >= source.width)
            {
                FIC2_CHECK(item.requests.size() == 1);
            }

            Item known { source.width, source.height };
            known.knownAspect = static_cast<float>(source.width) / static_cast<float>(source.height);
            for (int frame = 0; frame < 5; ++frame)
            {
                known.Frame(row);
            }
            FIC2_CHECK(known.requests.size() == 1);
            FIC2_CHECK(known.ReachesDisplayedSize(row));
        }
    }
}
