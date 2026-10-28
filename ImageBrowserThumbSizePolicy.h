#pragma once

#include "ImageCore/ImageRequest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Decode-size policy of strip thumbnails: which size to request, when an
// undersized thumbnail is decoded again, and whether a decoded (or cached)
// size satisfies the row height. Window-free; ImageBrowserThumbImage applies
// it to its binding.
//
// The strip shows every image at the row height and `rowHeight * aspect` wide
// (ThumbStripLayout::ImageExtent, no width cap), so what a thumbnail needs is
// its decoded height; the width follows from the aspect.
//
// Buckets: a request targets a bucket, the smallest of kBuckets (or, above
// the last, the next multiple of kBucketStepAbove) that is at least the row
// height. The request box is the bucket's square until the aspect is known,
// then `bucket * aspect` (plus a little slack, so the height is the side that
// binds) x `bucket`.
//
// Sufficiency compares the actual decoded size with the bucket: a payload is
// sufficient when its height reaches the bucket (within one pixel of
// rounding), or when the source limited it (neither decoded side reached its
// box side, ThumbPayloadCache::IsSourceLimited): a source with no more pixels
// satisfies every larger bucket. A payload at least as tall as the item's
// recorded source-limited height is sufficient too.
//
// Hysteresis: an upgrade is due only when the row height grows past the
// bucket of the payload held; a smaller row height never asks for a smaller
// decode (the held payload is kept and drawn smaller). With buckets 1.33x to
// 1.5x apart, a drag that reverses across a bucket boundary costs at most one
// decode, and that one serves both sides. While the row height is still
// moving (splitter drag, window size move), upgrades wait until it settles;
// a first decode (nothing to show yet) is never deferred.
//
// Requests: one request in flight per item binding (an upgrade is never
// canceled for a newer bucket; the next bucket is evaluated when it lands),
// and at most one upgrade per bucket and binding (UpgradeTracker), so an
// unexpectedly small result cannot loop.
namespace ThumbSizePolicy
{
    inline constexpr float kBuckets[] { 32.0f, 48.0f, 64.0f, 96.0f, 128.0f, 192.0f, 256.0f };
    inline constexpr size_t kBucketCount = sizeof(kBuckets) / sizeof(kBuckets[0]);
    inline constexpr float kBucketStepAbove = 64.0f;
    // Decoders round the scaled size: a side within this many pixels of its
    // target reaches it.
    inline constexpr float kRoundingTolerance = 1.0f;
    // Extra box width over `bucket * aspect`, for an aspect known only from a
    // bitmap's rounded size: a box whose height binds yields exactly the
    // bucket height.
    inline constexpr float kBoxWidthSlack = 0.02f;
    inline constexpr float kBoxWidthPad = 2.0f;

    // The bucket a row height requests (0 for no row height).
    float BucketFor(float rowHeight);
    // Index of `bucket` in the ladder (kBuckets, then the steps above it).
    size_t BucketIndex(float bucket);

    // The decode box for `bucket`: square while `aspect` (width / height) is
    // unknown (0), else wide enough that the height binds.
    ImageCore::Size RequestBox(float bucket, float aspect);

    // A decoded (or cached) payload as the policy sees it.
    struct Decoded
    {
        uint32_t width { 0 };
        uint32_t height { 0 };
        bool sourceLimited { false };

        bool Empty() const { return width == 0 || height == 0; }
        float Aspect() const
        {
            return Empty() ? 0.0f : static_cast<float>(width) / static_cast<float>(height);
        }
        // The largest source aspect this rounded decode can come from: an
        // upgrade box shaped by it binds on its height.
        float AspectUpperBound() const
        {
            return Empty() ? 0.0f
                : (static_cast<float>(width) + 0.5f) / (std::max)(static_cast<float>(height) - 0.5f, 0.5f);
        }
    };

    // What the item model knows of an item before its tile holds a payload.
    struct ItemHint
    {
        float aspect { 0.0f };                // width / height, 0 unknown
        uint32_t sourceLimitedHeight { 0 };   // a decode was source-limited at this height
    };

    // Whether `decoded` satisfies `bucket`. `sourceLimitedHeight` is the
    // item's recorded source-limited height (0: none).
    bool Satisfies(const Decoded& decoded, float bucket, uint32_t sourceLimitedHeight = 0);

    // Which cached size to show for `bucket`: the smallest one that satisfies
    // it (a larger one included), else the largest (shown until the upgrade
    // arrives; an undersized entry never satisfies a larger bucket).
    struct CacheChoice
    {
        size_t index { static_cast<size_t>(-1) };   // into the entries; none when empty
        bool satisfies { false };

        bool Found() const { return index != static_cast<size_t>(-1); }
    };
    CacheChoice ChooseCached(const std::vector<Decoded>& entries, float bucket, uint32_t sourceLimitedHeight = 0);

    // The upgrade buckets requested under one item binding.
    class UpgradeTracker
    {
    public:
        bool Requested(float bucket) const;
        void MarkRequested(float bucket);
        void Reset() { m_requested = 0; }
        size_t Count() const;

    private:
        uint64_t m_requested { 0 };   // bit i: bucket index i (capped at 63)
    };

    struct Input
    {
        float rowHeight { 0.0f };
        // Width / height known before any payload (item model or cache), 0
        // when unknown; the payload's own aspect wins once there is one.
        float knownAspect { 0.0f };
        Decoded shown {};                 // the payload held for the item (empty: none)
        uint32_t sourceLimitedHeight { 0 };
        bool requestInFlight { false };
        bool deferUpgrades { false };     // the row height is still moving
    };

    enum class Action
    {
        None,
        FirstDecode,   // nothing to show yet
        Upgrade,       // the held payload is undersized for the bucket
    };

    struct Decision
    {
        Action action { Action::None };
        float bucket { 0.0f };
        ImageCore::Size box { 0.0f, 0.0f };
        // An upgrade is due but waits (deferred, or this bucket was already
        // requested under the binding).
        bool upgradeWithheld { false };
    };

    Decision Evaluate(const Input& input, const UpgradeTracker& tracker);
}
