#include "ImageBrowserThumbSizePolicy.h"

#include <algorithm>
#include <cmath>

namespace ThumbSizePolicy
{
    float BucketFor(float rowHeight)
    {
        if (!(rowHeight > 0.0f))
        {
            return 0.0f;
        }
        for (const float bucket : kBuckets)
        {
            if (bucket >= rowHeight)
            {
                return bucket;
            }
        }
        const float last = kBuckets[kBucketCount - 1];
        return last + std::ceil((rowHeight - last) / kBucketStepAbove) * kBucketStepAbove;
    }

    size_t BucketIndex(float bucket)
    {
        for (size_t i = 0; i < kBucketCount; ++i)
        {
            if (bucket <= kBuckets[i])
            {
                return i;
            }
        }
        const float last = kBuckets[kBucketCount - 1];
        return kBucketCount - 1 + static_cast<size_t>(std::ceil((bucket - last) / kBucketStepAbove));
    }

    ImageCore::Size RequestBox(float bucket, float aspect)
    {
        if (!(bucket > 0.0f))
        {
            return { 0.0f, 0.0f };
        }
        if (!(aspect > 0.0f))
        {
            return { bucket, bucket };
        }
        const float width = std::ceil(bucket * aspect * (1.0f + kBoxWidthSlack) + kBoxWidthPad);
        return { width, bucket };
    }

    bool Satisfies(const Decoded& decoded, float bucket, uint32_t sourceLimitedHeight)
    {
        if (decoded.Empty())
        {
            return false;
        }
        if (decoded.sourceLimited)
        {
            return true;
        }
        if (sourceLimitedHeight > 0 && decoded.height >= sourceLimitedHeight)
        {
            return true;
        }
        return static_cast<float>(decoded.height) + kRoundingTolerance >= bucket;
    }

    CacheChoice ChooseCached(const std::vector<Decoded>& entries, float bucket, uint32_t sourceLimitedHeight)
    {
        CacheChoice best {};
        CacheChoice largest {};
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const Decoded& entry = entries[i];
            if (entry.Empty())
            {
                continue;
            }
            if (Satisfies(entry, bucket, sourceLimitedHeight) &&
                (!best.Found() || entry.height < entries[best.index].height))
            {
                best = { i, true };
            }
            if (!largest.Found() || entry.height > entries[largest.index].height)
            {
                largest = { i, false };
            }
        }
        return best.Found() ? best : largest;
    }

    bool UpgradeTracker::Requested(float bucket) const
    {
        const size_t index = (std::min)(BucketIndex(bucket), size_t { 63 });
        return (m_requested & (uint64_t { 1 } << index)) != 0;
    }

    void UpgradeTracker::MarkRequested(float bucket)
    {
        const size_t index = (std::min)(BucketIndex(bucket), size_t { 63 });
        m_requested |= (uint64_t { 1 } << index);
    }

    size_t UpgradeTracker::Count() const
    {
        size_t count = 0;
        for (uint64_t bits = m_requested; bits != 0; bits &= bits - 1)
        {
            ++count;
        }
        return count;
    }

    Decision Evaluate(const Input& input, const UpgradeTracker& tracker)
    {
        Decision decision {};
        decision.bucket = BucketFor(input.rowHeight);
        if (!(decision.bucket > 0.0f) || input.requestInFlight)
        {
            return decision;
        }

        if (input.shown.Empty())
        {
            decision.action = Action::FirstDecode;
            decision.box = RequestBox(decision.bucket, input.knownAspect);
            return decision;
        }

        if (Satisfies(input.shown, decision.bucket, input.sourceLimitedHeight))
        {
            return decision;
        }
        if (input.deferUpgrades || tracker.Requested(decision.bucket))
        {
            decision.upgradeWithheld = true;
            return decision;
        }
        decision.action = Action::Upgrade;
        decision.box = RequestBox(decision.bucket, input.shown.AspectUpperBound());
        return decision;
    }
}
