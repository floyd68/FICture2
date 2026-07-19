#include "ImageGpuResourceCache.h"

#include <algorithm>

ImageGpuResourceCache& ImageGpuResourceCache::Instance()
{
    static ImageGpuResourceCache instance;
    return instance;
}

void ImageGpuResourceCache::ClearUnlocked()
{
    m_cache.clear();
    m_lru.clear();
    m_bytesInUse = 0;
}

size_t ImageGpuResourceCache::EstimateBytes(UINT w, UINT h, DXGI_FORMAT format)
{
    auto blocks = [](UINT x) -> size_t
    {
        return (static_cast<size_t>(x) + 3) / 4;
    };

    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return blocks(w) * blocks(h) * 8;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return blocks(w) * blocks(h) * 16;
    default:
        return static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    }
}

void ImageGpuResourceCache::EnsureDeviceGenerationUnlocked(uint64_t deviceGeneration)
{
    if (m_deviceGeneration != deviceGeneration)
    {
        ClearUnlocked();
        m_deviceGeneration = deviceGeneration;
    }
}

void ImageGpuResourceCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ClearUnlocked();
}

bool ImageGpuResourceCache::TryGet(
    const std::wstring& path,
    uint32_t mipLevel,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
    UINT& outW,
    UINT& outH,
    DXGI_FORMAT& outFmt,
    ImageAlphaInfo& outAlpha,
    uint32_t& outSourceMipLevels,
    uint32_t& outSourceMipIndex,
    uint64_t deviceGeneration)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    EnsureDeviceGenerationUnlocked(deviceGeneration);

    const Key key { path, mipLevel };
    auto it = m_cache.find(key);
    if (it == m_cache.end() || !it->second.srv)
    {
        return false;
    }

    m_lru.erase(it->second.lruIt);
    m_lru.push_front(key);
    it->second.lruIt = m_lru.begin();

    outSrv = it->second.srv;
    outW = it->second.width;
    outH = it->second.height;
    outFmt = it->second.format;
    outAlpha = it->second.alpha;
    outSourceMipLevels = it->second.sourceMipLevels;
    outSourceMipIndex = it->second.sourceMipIndex;
    return true;
}

void ImageGpuResourceCache::Put(
    const std::wstring& path,
    uint32_t mipLevel,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
    UINT w,
    UINT h,
    DXGI_FORMAT format,
    const ImageAlphaInfo& alpha,
    uint32_t sourceMipLevels,
    uint32_t sourceMipIndex,
    uint64_t deviceGeneration)
{
    if (!srv)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    EnsureDeviceGenerationUnlocked(deviceGeneration);

    const Key key { path, mipLevel };
    if (auto it = m_cache.find(key); it != m_cache.end())
    {
        m_bytesInUse -= it->second.bytes;
        m_lru.erase(it->second.lruIt);
        m_cache.erase(it);
    }

    m_lru.push_front(key);
    Entry entry {};
    entry.srv = srv;
    entry.width = w;
    entry.height = h;
    entry.format = format;
    entry.alpha = alpha;
    entry.sourceMipLevels = (std::max)(1u, sourceMipLevels);
    entry.sourceMipIndex = sourceMipIndex;
    entry.bytes = EstimateBytes(w, h, format);
    entry.lruIt = m_lru.begin();
    m_bytesInUse += entry.bytes;
    m_cache.emplace(key, std::move(entry));

    while (m_cache.size() > 1 &&
        (m_cache.size() > m_capacity || m_bytesInUse > m_byteBudget) &&
        !m_lru.empty())
    {
        const Key victimKey = m_lru.back();
        m_lru.pop_back();
        if (auto vit = m_cache.find(victimKey); vit != m_cache.end())
        {
            m_bytesInUse -= vit->second.bytes;
            m_cache.erase(vit);
        }
    }
}
