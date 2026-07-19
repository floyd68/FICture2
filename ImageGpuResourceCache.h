#pragma once

#include "ImageAlphaPresentation.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

// FICture2-owned LRU cache of (path, mip) -> ID3D11ShaderResourceView.
// Cleared automatically when deviceGeneration changes between calls.
class ImageGpuResourceCache
{
public:
    static ImageGpuResourceCache& Instance();

    ImageGpuResourceCache(const ImageGpuResourceCache&) = delete;
    ImageGpuResourceCache& operator=(const ImageGpuResourceCache&) = delete;

    bool TryGet(
        const std::wstring& path,
        uint32_t mipLevel,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
        UINT& outW,
        UINT& outH,
        DXGI_FORMAT& outFmt,
        ImageAlphaInfo& outAlpha,
        uint32_t& outSourceMipLevels,
        uint32_t& outSourceMipIndex,
        uint64_t deviceGeneration);

    void Put(
        const std::wstring& path,
        uint32_t mipLevel,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
        UINT w,
        UINT h,
        DXGI_FORMAT format,
        const ImageAlphaInfo& alpha,
        uint32_t sourceMipLevels,
        uint32_t sourceMipIndex,
        uint64_t deviceGeneration);

    void Clear();

private:
    ImageGpuResourceCache() = default;

    void ClearUnlocked();
    void EnsureDeviceGenerationUnlocked(uint64_t deviceGeneration);
    static size_t EstimateBytes(UINT w, UINT h, DXGI_FORMAT format);

    struct Key
    {
        std::wstring path;
        uint32_t mipLevel { 0 };

        bool operator==(const Key& other) const
        {
            return mipLevel == other.mipLevel && path == other.path;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key& key) const
        {
            const size_t pathHash = std::hash<std::wstring> {}(key.path);
            const size_t mipHash = std::hash<uint32_t> {}(key.mipLevel);
            return pathHash ^ (mipHash + 0x9e3779b9u + (pathHash << 6) + (pathHash >> 2));
        }
    };

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
        UINT width { 0 };
        UINT height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        ImageAlphaInfo alpha {};
        uint32_t sourceMipLevels { 1 };
        uint32_t sourceMipIndex { 0 };
        size_t bytes { 0 };
        std::list<Key>::iterator lruIt {};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<Key, Entry, KeyHash> m_cache;
    std::list<Key> m_lru;
    size_t m_capacity { 64 };
    size_t m_byteBudget { 512ull * 1024 * 1024 };
    size_t m_bytesInUse { 0 };
    uint64_t m_deviceGeneration { 0 };
};
