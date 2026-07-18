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

// FICture2-owned LRU cache of path -> ID3D11ShaderResourceView.
// Cleared automatically when deviceGeneration changes between calls.
class ImageGpuResourceCache
{
public:
    static ImageGpuResourceCache& Instance();

    ImageGpuResourceCache(const ImageGpuResourceCache&) = delete;
    ImageGpuResourceCache& operator=(const ImageGpuResourceCache&) = delete;

    bool TryGet(
        const std::wstring& path,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
        UINT& outW,
        UINT& outH,
        DXGI_FORMAT& outFmt,
        ImageAlphaInfo& outAlpha,
        uint64_t deviceGeneration);

    void Put(
        const std::wstring& path,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
        UINT w,
        UINT h,
        DXGI_FORMAT format,
        const ImageAlphaInfo& alpha,
        uint64_t deviceGeneration);

    void Clear();

private:
    ImageGpuResourceCache() = default;

    void ClearUnlocked();
    void EnsureDeviceGenerationUnlocked(uint64_t deviceGeneration);

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
        UINT width { 0 };
        UINT height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        ImageAlphaInfo alpha {};
        std::list<std::wstring>::iterator lruIt {};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, Entry> m_cache;
    std::list<std::wstring> m_lru;
    size_t m_capacity { 64 };
    uint64_t m_deviceGeneration { 0 };
};
