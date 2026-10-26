#include "ImageBrowserThumbPayloadCache.h"

#include <algorithm>
#include <utility>

struct ThumbPayloadCache::Node
{
    ThumbPayloadKey key {};
    Payload payload {};
    ThumbPayloadInfo info {};
    size_t bytes { 0 };
    size_t pins { 0 };
    bool isEntry { false };   // keyed (and accounted as an entry), else held
    bool inLru { false };
    std::list<std::shared_ptr<Node>>::iterator lruPos {};
};

struct ThumbPayloadCache::Impl
{
    Limits limits {};
    // Keyed entries by normalized path (a few decoded sizes per path).
    std::unordered_map<std::wstring, std::vector<std::shared_ptr<Node>>> byPath {};
    // Unpinned entries, most recently released first.
    std::list<std::shared_ptr<Node>> lru {};
    Stats stats {};

    static bool Matches(const Node& node, const ThumbPayloadKey& key)
    {
        return node.key.identity == key.identity &&
            node.key.width == key.width &&
            node.key.height == key.height &&
            node.key.options == key.options;
    }

    std::shared_ptr<Node> FindNode(const ThumbPayloadKey& key) const
    {
        if (!IsCacheable(key.identity))
        {
            return nullptr;
        }
        const auto it = byPath.find(key.path);
        if (it == byPath.end())
        {
            return nullptr;
        }
        for (const auto& node : it->second)
        {
            if (Matches(*node, key))
            {
                return node;
            }
        }
        return nullptr;
    }

    void AddPin(Node& node)
    {
        if (node.pins++ == 0 && node.isEntry)
        {
            if (node.inLru)
            {
                lru.erase(node.lruPos);
                node.inLru = false;
            }
            ++stats.pinnedEntries;
            stats.pinnedBytes += node.bytes;
        }
    }

    void ReleasePin(const std::shared_ptr<Node>& node)
    {
        if (node->pins == 0 || --node->pins > 0)
        {
            return;
        }
        if (!node->isEntry)
        {
            stats.heldBytes -= node->bytes;
            return;
        }
        --stats.pinnedEntries;
        stats.pinnedBytes -= node->bytes;
        lru.push_front(node);
        node->lruPos = lru.begin();
        node->inLru = true;
        Trim();
    }

    void RemoveEntry(const std::shared_ptr<Node>& node)
    {
        auto it = byPath.find(node->key.path);
        if (it != byPath.end())
        {
            auto& nodes = it->second;
            nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());
            if (nodes.empty())
            {
                byPath.erase(it);
            }
        }
        if (node->inLru)
        {
            lru.erase(node->lruPos);
            node->inLru = false;
        }
        node->isEntry = false;
        --stats.entries;
        stats.entryBytes -= node->bytes;
    }

    // Evicts unpinned entries, least recently released first, until the
    // accounted total is within both bounds or only pinned payloads remain.
    void Trim()
    {
        while (!lru.empty() &&
            (stats.entryBytes + stats.heldBytes > limits.maxBytes || stats.entries > limits.maxEntries))
        {
            const std::shared_ptr<Node> victim = lru.back();
            RemoveEntry(victim);
            ++stats.evictions;
        }
    }
};

ThumbPayloadCache::Pin::Pin(std::shared_ptr<Impl> impl, std::shared_ptr<Node> node)
    : m_impl(std::move(impl))
    , m_node(std::move(node))
{
}

ThumbPayloadCache::Pin::Pin(Pin&& other) noexcept
    : m_impl(std::move(other.m_impl))
    , m_node(std::move(other.m_node))
{
}

ThumbPayloadCache::Pin& ThumbPayloadCache::Pin::operator=(Pin&& other) noexcept
{
    if (this != &other)
    {
        Release();
        m_impl = std::move(other.m_impl);
        m_node = std::move(other.m_node);
    }
    return *this;
}

ThumbPayloadCache::Pin::~Pin()
{
    Release();
}

ThumbPayloadCache::Pin ThumbPayloadCache::Pin::Detached(const Payload& payload)
{
    if (!payload.blocks)
    {
        return {};
    }
    auto node = std::make_shared<Node>();
    node->payload = payload;
    node->info.width = payload.width;
    node->info.height = payload.height;
    node->pins = 1;
    return Pin(nullptr, std::move(node));
}

const ThumbPayloadCache::Payload& ThumbPayloadCache::Pin::Get() const
{
    static const Payload kEmpty {};
    return m_node ? m_node->payload : kEmpty;
}

const ThumbPayloadInfo& ThumbPayloadCache::Pin::Info() const
{
    static const ThumbPayloadInfo kNone {};
    return m_node ? m_node->info : kNone;
}

bool ThumbPayloadCache::Pin::IsEntry() const
{
    return m_node && m_node->isEntry;
}

void ThumbPayloadCache::Pin::Release()
{
    std::shared_ptr<Node> node = std::move(m_node);
    std::shared_ptr<Impl> impl = std::move(m_impl);
    m_node.reset();
    m_impl.reset();
    if (node && impl)
    {
        impl->ReleasePin(node);
    }
}

ThumbPayloadCache::ThumbPayloadCache(Limits limits)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->limits = limits;
}

ThumbPayloadCache::~ThumbPayloadCache() = default;

const std::shared_ptr<ThumbPayloadCache>& ThumbPayloadCache::Shared()
{
    static const std::shared_ptr<ThumbPayloadCache> cache = std::make_shared<ThumbPayloadCache>();
    return cache;
}

bool ThumbPayloadCache::IsSourceLimited(uint32_t width, uint32_t height, const ImageCore::Size& requestBox)
{
    // A side reaches its box side when it is within one pixel of it (the
    // decoder rounds the scaled size); a side without a box always does.
    const auto reaches = [](uint32_t decoded, float box)
    {
        return !(box > 0.0f) || static_cast<float>(decoded) + 1.0f >= box;
    };
    return !reaches(width, requestBox.w) && !reaches(height, requestBox.h);
}

ThumbPayloadCache::Pin ThumbPayloadCache::Insert(
    const std::wstring& path,
    const ImageSourceIdentity& identity,
    const ThumbDecodeOptions& options,
    const ImageCore::Size& requestBox,
    const Payload& payload)
{
    if (!payload.blocks || payload.width == 0 || payload.height == 0)
    {
        return {};
    }
    Impl& impl = *m_impl;

    ThumbPayloadInfo info {};
    info.width = payload.width;
    info.height = payload.height;
    info.requestBox = requestBox;
    info.sourceLimited = IsSourceLimited(payload.width, payload.height, requestBox);

    ThumbPayloadKey key {};
    key.path = path;
    key.identity = identity;
    key.width = payload.width;
    key.height = payload.height;
    key.options = options;

    if (std::shared_ptr<Node> existing = impl.FindNode(key))
    {
        // Same key, same pixels: keep the entry. A source-limited result is
        // the stronger statement about the source, so it wins.
        if (info.sourceLimited && !existing->info.sourceLimited)
        {
            existing->info = info;
        }
        impl.AddPin(*existing);
        return Pin(m_impl, std::move(existing));
    }

    auto node = std::make_shared<Node>();
    node->key = std::move(key);
    node->payload = payload;
    node->info = info;
    node->bytes = payload.blocks->size();

    const bool entry = IsCacheable(identity) && !path.empty() && node->bytes <= impl.limits.maxEntryBytes;
    if (entry)
    {
        node->isEntry = true;
        impl.byPath[node->key.path].push_back(node);
        ++impl.stats.entries;
        impl.stats.entryBytes += node->bytes;
        impl.AddPin(*node);
    }
    else
    {
        node->pins = 1;
        impl.stats.heldBytes += node->bytes;
    }
    // The new payload is pinned: only older unpinned entries make room.
    impl.Trim();
    return Pin(m_impl, std::move(node));
}

ThumbPayloadCache::Pin ThumbPayloadCache::Acquire(const ThumbPayloadKey& key)
{
    std::shared_ptr<Node> node = m_impl->FindNode(key);
    if (!node)
    {
        return {};
    }
    m_impl->AddPin(*node);
    return Pin(m_impl, std::move(node));
}

std::vector<ThumbPayloadInfo> ThumbPayloadCache::Find(
    const std::wstring& path,
    const ImageSourceIdentity& identity,
    const ThumbDecodeOptions& options) const
{
    std::vector<ThumbPayloadInfo> found;
    if (!IsCacheable(identity))
    {
        return found;
    }
    const auto it = m_impl->byPath.find(path);
    if (it == m_impl->byPath.end())
    {
        return found;
    }
    for (const auto& node : it->second)
    {
        if (node->key.identity == identity && node->key.options == options)
        {
            found.push_back(node->info);
        }
    }
    return found;
}

ThumbPayloadCache::Pin ThumbPayloadCache::AcquireLargest(
    const std::wstring& path,
    const ImageSourceIdentity& identity,
    const ThumbDecodeOptions& options)
{
    const std::vector<ThumbPayloadInfo> sizes = Find(path, identity, options);
    if (sizes.empty())
    {
        return {};
    }
    const auto largest = std::max_element(sizes.begin(), sizes.end(),
        [](const ThumbPayloadInfo& a, const ThumbPayloadInfo& b)
        {
            return static_cast<uint64_t>(a.width) * a.height < static_cast<uint64_t>(b.width) * b.height;
        });
    ThumbPayloadKey key {};
    key.path = path;
    key.identity = identity;
    key.width = largest->width;
    key.height = largest->height;
    key.options = options;
    return Acquire(key);
}

const ThumbPayloadCache::Limits& ThumbPayloadCache::GetLimits() const
{
    return m_impl->limits;
}

ThumbPayloadCache::Stats ThumbPayloadCache::GetStats() const
{
    return m_impl->stats;
}

size_t ThumbPayloadCache::TotalBytes() const
{
    return m_impl->stats.entryBytes + m_impl->stats.heldBytes;
}

size_t ThumbPayloadCache::EntryCount() const
{
    return m_impl->stats.entries;
}
