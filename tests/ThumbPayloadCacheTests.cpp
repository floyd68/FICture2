#include "TestHarness.h"

#include "ImageBrowserThumbPayloadCache.h"

#include <memory>
#include <string>
#include <vector>

// The bounded thumbnail payload cache on its own: key (path, listed identity,
// decoded size, decode options), entry metadata (request box, source-limited),
// LRU eviction within byte and count bounds, and how pinned, held (unknown
// identity) and oversize payloads are accounted.

namespace
{
    const std::wstring kA = L"c:\\pics\\a.png";
    const std::wstring kB = L"c:\\pics\\b.png";
    const std::wstring kC = L"c:\\pics\\c.png";
    const std::wstring kD = L"c:\\pics\\d.png";
    const ThumbDecodeOptions kOptions {};
    const ImageCore::Size kBox { 128.0f, 128.0f };

    ImageSourceIdentity Identity(uint64_t size, uint64_t writeTime)
    {
        ImageSourceIdentity identity {};
        identity.size = size;
        identity.writeTime = writeTime;
        identity.known = true;
        return identity;
    }

    const ImageSourceIdentity kId = Identity(1000, 1);

    ThumbPayloadCache::Payload MakePayload(const std::wstring& path, uint32_t width, uint32_t height)
    {
        ThumbPayloadCache::Payload payload {};
        payload.width = width;
        payload.height = height;
        payload.rowPitch = width * 4u;
        payload.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        payload.blocks = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4u, uint8_t(0x40));
        payload.sourcePath = path;
        return payload;
    }

    size_t Bytes(uint32_t width, uint32_t height)
    {
        return static_cast<size_t>(width) * height * 4u;
    }

    // Inserts and drops the pin at once: an evictable entry.
    void Retain(ThumbPayloadCache& cache, const std::wstring& path, uint32_t width, uint32_t height,
        const ImageSourceIdentity& identity = kId)
    {
        (void)cache.Insert(path, identity, kOptions, kBox, MakePayload(path, width, height));
    }

    bool Has(const ThumbPayloadCache& cache, const std::wstring& path, const ImageSourceIdentity& identity = kId)
    {
        return !cache.Find(path, identity, kOptions).empty();
    }

    ThumbPayloadKey Key(const std::wstring& path, uint32_t width, uint32_t height,
        const ImageSourceIdentity& identity = kId)
    {
        ThumbPayloadKey key {};
        key.path = path;
        key.identity = identity;
        key.width = width;
        key.height = height;
        key.options = kOptions;
        return key;
    }
}

FIC2_TEST(ThumbPayloadCache_EvictsLeastRecentlyReleasedWithinCountAndBytes)
{
    // Count bound: three entries of 8 x 8 (256 bytes each).
    ThumbPayloadCache cache({ 1u << 20, 3, 1u << 20 });
    Retain(cache, kA, 8, 8);
    Retain(cache, kB, 8, 8);
    Retain(cache, kC, 8, 8);
    FIC2_CHECK(cache.EntryCount() == 3);
    FIC2_CHECK(cache.TotalBytes() == 3 * Bytes(8, 8));

    // Using A (pin, then release) makes it the most recent: B is evicted next.
    cache.Acquire(Key(kA, 8, 8)).Release();
    Retain(cache, kD, 8, 8);
    FIC2_CHECK(cache.EntryCount() == 3);
    FIC2_CHECK(!Has(cache, kB));
    FIC2_CHECK(Has(cache, kA) && Has(cache, kC) && Has(cache, kD));
    FIC2_CHECK(cache.GetStats().evictions == 1);

    // Then C, then A (least recently released first).
    Retain(cache, kB, 8, 8);
    FIC2_CHECK(!Has(cache, kC) && Has(cache, kA));
    Retain(cache, kC, 8, 8);
    FIC2_CHECK(!Has(cache, kA) && Has(cache, kD) && Has(cache, kB) && Has(cache, kC));

    // Byte bound: room for 1,000 bytes. A 16 x 8 entry (512 bytes) leaves
    // room for one 8 x 8 entry next to it.
    ThumbPayloadCache bytes({ 1000, 100, 1000 });
    Retain(bytes, kA, 8, 8);
    Retain(bytes, kB, 8, 8);
    Retain(bytes, kC, 8, 8);
    FIC2_CHECK(bytes.EntryCount() == 3 && bytes.TotalBytes() == 768);
    Retain(bytes, kD, 16, 8);
    FIC2_CHECK(bytes.TotalBytes() <= 1000);
    FIC2_CHECK(bytes.EntryCount() == 2);
    FIC2_CHECK(!Has(bytes, kA) && !Has(bytes, kB) && Has(bytes, kC) && Has(bytes, kD));
    FIC2_CHECK(bytes.GetStats().evictions == 2);
}

FIC2_TEST(ThumbPayloadCache_PinnedEntriesAreAccountedAndNeverEvicted)
{
    ThumbPayloadCache cache({ 1000, 2, 1000 });
    ThumbPayloadCache::Pin a = cache.Insert(kA, kId, kOptions, kBox, MakePayload(kA, 8, 8));
    ThumbPayloadCache::Pin b = cache.Insert(kB, kId, kOptions, kBox, MakePayload(kB, 8, 8));
    Retain(cache, kC, 8, 8);   // over the count with A and B pinned: C goes at once
    FIC2_CHECK(!Has(cache, kC));
    FIC2_CHECK(cache.EntryCount() == 2);

    // Every entry pinned: a third pinned payload still goes in (its tile holds
    // it anyway) and the cache reports being over its bounds by pinned bytes.
    ThumbPayloadCache::Pin c = cache.Insert(kC, kId, kOptions, kBox, MakePayload(kC, 8, 8));
    FIC2_CHECK(c.IsEntry());
    FIC2_CHECK(cache.EntryCount() == 3);
    const ThumbPayloadCache::Stats pinned = cache.GetStats();
    FIC2_CHECK(pinned.pinnedEntries == 3);
    FIC2_CHECK(pinned.pinnedBytes == 3 * Bytes(8, 8));
    FIC2_CHECK(cache.TotalBytes() == pinned.pinnedBytes);
    FIC2_CHECK(a.Get().blocks && b.Get().blocks && c.Get().blocks);

    // Released pins become evictable and the cache trims back to its bounds.
    a.Release();
    FIC2_CHECK(cache.EntryCount() == 2);
    FIC2_CHECK(!Has(cache, kA));
    b.Release();
    c.Release();
    FIC2_CHECK(cache.EntryCount() == 2);
    FIC2_CHECK(cache.GetStats().pinnedEntries == 0);
    FIC2_CHECK(cache.TotalBytes() <= 1000);

    // A pin outlives the cache object (the payload stays valid).
    ThumbPayloadCache::Pin survivor;
    {
        ThumbPayloadCache scoped({ 1000, 2, 1000 });
        survivor = scoped.Insert(kA, kId, kOptions, kBox, MakePayload(kA, 4, 4));
    }
    FIC2_CHECK(survivor.Get().width == 4);
    survivor.Release();
}

FIC2_TEST(ThumbPayloadCache_OversizeAndUnknownIdentityAreHeldNotCached)
{
    ThumbPayloadCache cache({ 1000, 10, 300 });
    Retain(cache, kA, 8, 8);
    Retain(cache, kB, 8, 8);
    FIC2_REQUIRE(cache.EntryCount() == 2);

    // Oversize (16 x 8 = 512 bytes > 300): held while pinned, never an entry,
    // and its bytes count: the unpinned entries make room.
    {
        ThumbPayloadCache::Pin big = cache.Insert(kC, kId, kOptions, kBox, MakePayload(kC, 16, 8));
        FIC2_REQUIRE(big);
        FIC2_CHECK(!big.IsEntry());
        FIC2_CHECK(!Has(cache, kC));
        FIC2_CHECK(cache.GetStats().heldBytes == 512);
        FIC2_CHECK(cache.TotalBytes() <= 1000);
        FIC2_CHECK(cache.EntryCount() == 1);   // A evicted for it
        FIC2_CHECK(big.Get().width == 16);
    }
    FIC2_CHECK(cache.GetStats().heldBytes == 0);
    FIC2_CHECK(!Has(cache, kC));

    // Unknown identity: never inserted, never matched (not even by itself).
    const ImageSourceIdentity unknown {};
    ThumbPayloadCache::Pin held = cache.Insert(kD, unknown, kOptions, kBox, MakePayload(kD, 4, 4));
    FIC2_REQUIRE(held);
    FIC2_CHECK(!held.IsEntry());
    FIC2_CHECK(cache.GetStats().heldBytes == Bytes(4, 4));
    FIC2_CHECK(cache.Find(kD, unknown, kOptions).empty());
    FIC2_CHECK(!cache.Acquire(Key(kD, 4, 4, unknown)));
    FIC2_CHECK(!cache.AcquireLargest(kD, unknown, kOptions));
    ThumbPayloadCache::Pin again = cache.Insert(kD, unknown, kOptions, kBox, MakePayload(kD, 4, 4));
    FIC2_CHECK(!again.IsEntry());
    FIC2_CHECK(cache.GetStats().heldBytes == 2 * Bytes(4, 4));
    held.Release();
    again.Release();
    FIC2_CHECK(cache.GetStats().heldBytes == 0);
    FIC2_CHECK(cache.Find(kD, unknown, kOptions).empty());

    // Without a cache a payload is held by its owner alone.
    ThumbPayloadCache::Pin detached = ThumbPayloadCache::Pin::Detached(MakePayload(kA, 2, 2));
    FIC2_CHECK(detached && !detached.IsEntry() && detached.Get().width == 2);
}

FIC2_TEST(ThumbPayloadCache_LookupsReturnTheRequestBoxAndSourceLimitedFlag)
{
    ThumbPayloadCache cache;

    // Box-limited: a 4:3 source fitted into a 128 box reaches its width.
    Retain(cache, kA, 128, 96);
    // Source-limited: a 100 x 50 source does not reach either side of a 128 box.
    Retain(cache, kB, 100, 50);
    // Within rounding: 127 of a 128 box still reaches it.
    Retain(cache, kC, 127, 40);

    const auto a = cache.Find(kA, kId, kOptions);
    FIC2_REQUIRE(a.size() == 1);
    FIC2_CHECK(a[0].width == 128 && a[0].height == 96);
    FIC2_CHECK(a[0].requestBox.w == 128.0f && a[0].requestBox.h == 128.0f);
    FIC2_CHECK(!a[0].sourceLimited);
    const auto b = cache.Find(kB, kId, kOptions);
    FIC2_REQUIRE(b.size() == 1);
    FIC2_CHECK(b[0].sourceLimited);
    FIC2_CHECK(b[0].requestBox.w == 128.0f);
    FIC2_CHECK(!cache.Find(kC, kId, kOptions).at(0).sourceLimited);

    // The pin of an exact key carries the same metadata.
    ThumbPayloadCache::Pin pinB = cache.Acquire(Key(kB, 100, 50));
    FIC2_REQUIRE(pinB);
    FIC2_CHECK(pinB.Info().sourceLimited);
    FIC2_CHECK(pinB.Get().width == 100);
    pinB.Release();

    // The same source at another decoded size is a second entry; a lookup by
    // path and identity lists both, and the largest can be pinned.
    const ImageCore::Size bigBox { 256.0f, 256.0f };
    (void)cache.Insert(kA, kId, kOptions, bigBox, MakePayload(kA, 256, 192));
    const auto sizes = cache.Find(kA, kId, kOptions);
    FIC2_CHECK(sizes.size() == 2);
    ThumbPayloadCache::Pin largest = cache.AcquireLargest(kA, kId, kOptions);
    FIC2_REQUIRE(largest);
    FIC2_CHECK(largest.Info().width == 256 && largest.Info().requestBox.w == 256.0f);
    FIC2_CHECK(cache.Acquire(Key(kA, 128, 96)).Info().requestBox.w == 128.0f);

    // A box-limited entry decoded again at the same size from a larger box
    // (the source had no more pixels) becomes source-limited.
    FIC2_CHECK(!cache.Find(kC, kId, kOptions).at(0).sourceLimited);
    (void)cache.Insert(kC, kId, kOptions, bigBox, MakePayload(kC, 127, 40));
    const auto c = cache.Find(kC, kId, kOptions);
    FIC2_REQUIRE(c.size() == 1);
    FIC2_CHECK(c[0].sourceLimited && c[0].requestBox.w == 256.0f);

    // Decode options that change pixels are part of the key.
    ThumbDecodeOptions otherOptions {};
    otherOptions.mipLevel = 1;
    FIC2_CHECK(cache.Find(kA, kId, otherOptions).empty());

    // Unbounded sides: no box side means it is reached.
    FIC2_CHECK(!ThumbPayloadCache::IsSourceLimited(10, 10, { 0.0f, 0.0f }));
    FIC2_CHECK(ThumbPayloadCache::IsSourceLimited(10, 10, { 64.0f, 64.0f }));
    FIC2_CHECK(!ThumbPayloadCache::IsSourceLimited(64, 10, { 64.0f, 64.0f }));
    FIC2_CHECK(!ThumbPayloadCache::IsSourceLimited(10, 64, { 64.0f, 64.0f }));
}

FIC2_TEST(ThumbPayloadCache_ReplacedSourceOrArchiveHostIsAMiss)
{
    ThumbPayloadCache cache;

    // A file replaced at the same path is listed again with another identity:
    // its old entry is not served.
    Retain(cache, kA, 8, 8, Identity(1000, 1));
    FIC2_CHECK(Has(cache, kA, Identity(1000, 1)));
    FIC2_CHECK(!Has(cache, kA, Identity(1000, 2)));   // rewritten, same size
    FIC2_CHECK(!Has(cache, kA, Identity(1001, 1)));
    FIC2_CHECK(!cache.Acquire(Key(kA, 8, 8, Identity(1000, 2))));

    // Archive entries carry the host archive's identity: a rewritten archive
    // misses for every entry in it.
    const std::wstring entry1 = L"c:\\pics\\pack.zip\\one.png";
    const std::wstring entry2 = L"c:\\pics\\pack.zip\\two.png";
    const ImageSourceIdentity host = Identity(50000, 7);
    const ImageSourceIdentity rewritten = Identity(50000, 8);
    Retain(cache, entry1, 8, 8, host);
    Retain(cache, entry2, 8, 8, host);
    FIC2_CHECK(Has(cache, entry1, host) && Has(cache, entry2, host));
    FIC2_CHECK(!Has(cache, entry1, rewritten) && !Has(cache, entry2, rewritten));

    // The new identity's decode is a separate entry; the old ones age out.
    Retain(cache, entry1, 8, 8, rewritten);
    FIC2_CHECK(cache.Find(entry1, rewritten, kOptions).size() == 1);
    FIC2_CHECK(cache.Find(entry1, host, kOptions).size() == 1);
}
