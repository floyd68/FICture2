#pragma once

#include "ImageAsyncBinding.h"
#include "ImageCore/ImageRequest.h"
#include "ImageSourceIdentity.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Bounded cache of decoded thumbnail payloads (CPU pixels only), shared by
// every image browser. A thumbnail tile bound to an item shows a cached
// payload instead of requesting a new decode; only its target-bound D2D bitmap
// is rebuilt. D2D bitmaps are never cached here: each tile keeps one, created
// from the payload on bind, device loss or target recreation.
//
// Key: normalized source path, the source identity the item was listed with
// (ThumbModelEntry::identity), the actual decoded size and the decode options
// that change pixels. Lookups never read the file system; a file replaced at
// the same path gets a new identity when it is listed again, and the old
// entries age out. An unknown identity never equals another one, so such a
// payload is never inserted (nor matched): it is only accounted while held.
// Each entry also records the request box it was decoded for and whether the
// source limited the result (entry metadata, not part of the key): ImageCore
// returns only the final scaled size, so this is the only way to tell a
// box-limited decode from a source that has no more pixels.
//
// Ownership and bounds: a tile holds a Pin on its item's payload from the
// first frame after the decode completes (whether or not the tile renders:
// offscreen prefetch tiles are admitted too) until it is recycled. The
// payload is the tile's only CPU copy; its bitmap and device-loss recovery
// are both created from it. Until that frame a completed decode is staged in
// its tile's binding, at most one per bound tile. Every byte reachable
// through a pin or an entry is accounted in TotalBytes():
//   - evictable entries (not pinned) form the LRU;
//   - pinned entries are never evicted while pinned;
//   - held payloads that are not entries (unknown identity, oversize) count
//     until their last pin is released, then they are freed.
// After every insert and release, unpinned entries are evicted (least
// recently released first) until TotalBytes() <= maxBytes and
// EntryCount() <= maxEntries, or until no unpinned entry is left. Pinned
// payloads are therefore not bounded by the cache budget but by the number of
// tiles that pin them (the materialized tile range, #46) times one payload
// each; total memory is at most max(maxBytes, pinned + held bytes).
// An oversize payload (more than maxEntryBytes) is held while pinned but never
// kept as an evictable entry, so it cannot flush the cache. A bound tile keeps
// its pin until it is recycled, so neither a saturated cache nor an oversize
// payload makes it decode its item again (no reload loop).
//
// UI thread only (tiles and browsers run on it).
struct ThumbDecodeOptions
{
    bool srgb { true };
    bool allowGpuCompressedDDS { false };
    uint32_t mipLevel { 0 };

    // The request fields, other than source and size, that change pixels.
    static ThumbDecodeOptions Of(const ImageCore::ImageRequest& request)
    {
        return { request.srgb, request.allowGpuCompressedDDS, request.mipLevel };
    }

    bool operator==(const ThumbDecodeOptions&) const = default;
};

struct ThumbPayloadKey
{
    std::wstring path {};                // normalized source path
    ImageSourceIdentity identity {};     // as listed
    uint32_t width { 0 };                // actual decoded size
    uint32_t height { 0 };
    ThumbDecodeOptions options {};
};

// What a lookup returns besides the pixels.
struct ThumbPayloadInfo
{
    uint32_t width { 0 };                // actual decoded size
    uint32_t height { 0 };
    ImageCore::Size requestBox { 0.0f, 0.0f };   // the box it was decoded for
    // Neither decoded side reaches the corresponding box side (within one
    // pixel of rounding): the source has no more pixels to give.
    bool sourceLimited { false };
};

class ThumbPayloadCache
{
public:
    using Payload = ImageAsyncBinding::Payload;

    struct Limits
    {
        size_t maxBytes { 0 };
        size_t maxEntries { 0 };
        size_t maxEntryBytes { 0 };   // larger payloads are oversize
    };

    // 256 MiB holds about 2,300 thumbnails of 228 x 128 (a 16:9 image at the
    // default row height) or 1,000 at 256 x 256.
    static constexpr Limits kDefaultLimits { 256u << 20, 4096, 32u << 20 };

    struct Stats
    {
        size_t entries { 0 };          // keyed entries, pinned or not
        size_t pinnedEntries { 0 };
        size_t entryBytes { 0 };       // bytes of keyed entries
        size_t pinnedBytes { 0 };      // of which pinned
        size_t heldBytes { 0 };        // pinned payloads that are not entries
        size_t evictions { 0 };        // entries evicted so far
    };

private:
    struct Node;
    struct Impl;

public:
    // A hold on one payload: while any pin on it lives, the payload is neither
    // evicted nor freed. Move-only; releasing the last pin makes an entry
    // evictable (most recently used) and frees a payload that is not one.
    class Pin
    {
    public:
        Pin() = default;
        Pin(Pin&& other) noexcept;
        Pin& operator=(Pin&& other) noexcept;
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        ~Pin();

        // A payload held by its owner alone (no cache): accounted nowhere.
        static Pin Detached(const Payload& payload);

        explicit operator bool() const { return m_node != nullptr; }
        // The pinned payload (valid while the pin is).
        const Payload& Get() const;
        const ThumbPayloadInfo& Info() const;
        // True when the payload is a keyed entry (lookups can find it).
        bool IsEntry() const;
        void Release();

    private:
        friend class ThumbPayloadCache;
        Pin(std::shared_ptr<Impl> impl, std::shared_ptr<Node> node);

        std::shared_ptr<Impl> m_impl {};
        std::shared_ptr<Node> m_node {};
    };

    explicit ThumbPayloadCache(Limits limits = kDefaultLimits);
    ~ThumbPayloadCache();
    ThumbPayloadCache(const ThumbPayloadCache&) = delete;
    ThumbPayloadCache& operator=(const ThumbPayloadCache&) = delete;

    // The application's cache (default limits), shared by every browser.
    static const std::shared_ptr<ThumbPayloadCache>& Shared();

    // Whether a payload of `identity` can be an entry (the identity is known).
    static bool IsCacheable(const ImageSourceIdentity& identity) { return identity.known; }
    static bool IsSourceLimited(uint32_t width, uint32_t height, const ImageCore::Size& requestBox);

    // Stores `payload`, decoded for `requestBox`, as the entry of (path,
    // identity, payload size, options) and returns a pin on it. An existing
    // entry of that key is pinned instead (same pixels); its metadata becomes
    // source-limited if this decode was. A payload that cannot be an entry
    // (unknown identity, oversize) is held only by the returned pin. Dropping
    // the pin at once retains an entry as evictable. Empty for an empty payload.
    Pin Insert(
        const std::wstring& path,
        const ImageSourceIdentity& identity,
        const ThumbDecodeOptions& options,
        const ImageCore::Size& requestBox,
        const Payload& payload);

    // The entry of `key`, pinned; empty when there is none.
    Pin Acquire(const ThumbPayloadKey& key);
    // The decoded sizes cached for (path, identity, options), with their
    // metadata; none for an unknown identity.
    std::vector<ThumbPayloadInfo> Find(
        const std::wstring& path,
        const ImageSourceIdentity& identity,
        const ThumbDecodeOptions& options) const;
    // The largest cached decoded size of (path, identity, options), pinned;
    // empty when there is none.
    Pin AcquireLargest(
        const std::wstring& path,
        const ImageSourceIdentity& identity,
        const ThumbDecodeOptions& options);

    const Limits& GetLimits() const;
    Stats GetStats() const;
    // Every accounted byte: entries (pinned or not) plus held payloads.
    size_t TotalBytes() const;
    size_t EntryCount() const;

private:
    std::shared_ptr<Impl> m_impl {};
};
