#include "TestHarness.h"

#include "ImageGpuResourceCache.h"
#include "ImagePresentationRecovery.h"
#include "ImageSourceIdentity.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <string>

// GPU presentation lifetime: source identity validates cached GPU content,
// the cache invalidates by path and device generation and releases its SRVs
// (and so its hold on the device) when cleared, and the main image's recovery
// decision follows the presentation's own resource domain.

using Microsoft::WRL::ComPtr;

namespace
{
    std::filesystem::path TempDir()
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const auto dir = std::filesystem::path(tempPath) /
            (L"FICture2GpuTests_" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        return dir;
    }

    void WriteBytes(const std::filesystem::path& path, size_t count, char value)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::string data(count, value);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    void SetWriteTime(const std::filesystem::path& path, uint64_t fileTime)
    {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        FILETIME ft { static_cast<DWORD>(fileTime & 0xFFFFFFFFu), static_cast<DWORD>(fileTime >> 32) };
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }

    struct WarpTexture
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11ShaderResourceView> srv;

        bool Create()
        {
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            {
                return false;
            }
            return MakeSrv(srv);
        }

        bool MakeSrv(ComPtr<ID3D11ShaderResourceView>& out) const
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = 4;
            desc.Height = 4;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
                SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &out));
        }

        // Current reference count of the device (AddRef/Release round trip).
        ULONG DeviceRefs() const
        {
            device->AddRef();
            return device->Release();
        }
    };

    bool Lookup(const std::wstring& path, uint32_t mip, const ImageSourceIdentity& identity, uint64_t generation)
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT w = 0;
        UINT h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        ImageAlphaInfo alpha {};
        uint32_t levels = 0;
        uint32_t index = 0;
        return ImageGpuResourceCache::Instance().TryGet(path, mip, srv, w, h, format, alpha, levels, index,
            identity, generation);
    }

    void Store(const std::wstring& path, uint32_t mip, const ComPtr<ID3D11ShaderResourceView>& srv,
        const ImageSourceIdentity& identity, uint64_t generation)
    {
        ImageGpuResourceCache::Instance().Put(path, mip, srv, 4, 4, DXGI_FORMAT_B8G8R8A8_UNORM, ImageAlphaInfo {},
            1, mip, identity, generation);
    }
}

FIC2_TEST(GpuPresentation_SourceIdentityTracksFileReplacement)
{
    const auto dir = TempDir();
    const auto file = dir / L"image.dds";
    WriteBytes(file, 100, 'a');
    SetWriteTime(file, 0x01D0000000000000ull);
    const ImageSourceIdentity original = ImageSourceIdentity::ForPath(file.wstring());
    FIC2_REQUIRE(original.known);
    FIC2_CHECK(original == ImageSourceIdentity::ForPath(file.wstring()));

    // Same size, newer write time (replaced in place).
    WriteBytes(file, 100, 'b');
    SetWriteTime(file, 0x01D0000000001000ull);
    FIC2_CHECK(original != ImageSourceIdentity::ForPath(file.wstring()));

    // Different size, same write time.
    WriteBytes(file, 120, 'b');
    SetWriteTime(file, 0x01D0000000000000ull);
    FIC2_CHECK(original != ImageSourceIdentity::ForPath(file.wstring()));

    // A missing file is unknown and never validates anything, even itself.
    const ImageSourceIdentity missing = ImageSourceIdentity::ForPath((dir / L"missing.dds").wstring());
    FIC2_CHECK(!missing.known);
    FIC2_CHECK(missing != missing);

    // An archive entry follows its host archive.
    const auto archive = dir / L"pack.zip";
    WriteBytes(archive, 64, 'z');
    const std::wstring entry = archive.wstring() + L"\\textures\\inner.dds";
    const ImageSourceIdentity entryIdentity = ImageSourceIdentity::ForPath(entry);
    FIC2_CHECK(entryIdentity.known);
    FIC2_CHECK(entryIdentity == ImageSourceIdentity::ForFile(archive.wstring()));
    WriteBytes(archive, 80, 'z');
    FIC2_CHECK(entryIdentity != ImageSourceIdentity::ForPath(entry));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FIC2_TEST(GpuPresentation_CacheRejectsReplacedSourceAndInvalidatesPath)
{
    WarpTexture gpu;
    FIC2_REQUIRE(gpu.Create());
    auto& cache = ImageGpuResourceCache::Instance();
    cache.Clear();

    const std::wstring path = L"c:/pics/a.dds";
    const ImageSourceIdentity v1 { 100, 1, true };
    const ImageSourceIdentity v2 { 100, 2, true };
    const uint64_t generation = 7;

    Store(path, 0, gpu.srv, v1, generation);
    Store(path, 1, gpu.srv, v1, generation);
    FIC2_CHECK(cache.EntryCount() == 2);
    FIC2_CHECK(Lookup(path, 0, v1, generation));

    // The file was replaced: the stale entry misses and is evicted.
    FIC2_CHECK(!Lookup(path, 0, v2, generation));
    FIC2_CHECK(cache.EntryCount() == 1);
    // Unknown identity (file gone) never hits.
    FIC2_CHECK(!Lookup(path, 1, ImageSourceIdentity {}, generation));

    // Explicit invalidation drops every mip of the path.
    Store(path, 0, gpu.srv, v2, generation);
    Store(path, 2, gpu.srv, v2, generation);
    Store(L"c:/pics/b.dds", 0, gpu.srv, v2, generation);
    cache.InvalidatePath(path);
    FIC2_CHECK(cache.EntryCount() == 1);
    FIC2_CHECK(Lookup(L"c:/pics/b.dds", 0, v2, generation));

    // Content with an unknown source is not cached at all.
    Store(L"c:/pics/c.dds", 0, gpu.srv, ImageSourceIdentity {}, generation);
    FIC2_CHECK(cache.EntryCount() == 1);

    // A new device generation clears everything from the old device.
    FIC2_CHECK(!Lookup(L"c:/pics/b.dds", 0, v2, generation + 1));
    FIC2_CHECK(cache.EntryCount() == 0);
    cache.Clear();
}

FIC2_TEST(GpuPresentation_ClearedCacheKeepsNoDeviceAlive)
{
    WarpTexture gpu;
    FIC2_REQUIRE(gpu.Create());
    auto& cache = ImageGpuResourceCache::Instance();
    cache.Clear();

    const ULONG baseline = gpu.DeviceRefs();
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        FIC2_REQUIRE(gpu.MakeSrv(srv));
        Store(L"c:/pics/lifetime.dds", 0, srv, ImageSourceIdentity { 1, 1, true }, 3);
    }
    // Only the cache still holds the SRV (and through it the device).
    FIC2_CHECK(cache.EntryCount() == 1);
    FIC2_CHECK(gpu.DeviceRefs() > baseline);

    // Application shutdown / device replacement: nothing cached keeps the device.
    cache.Clear();
    FIC2_CHECK(cache.EntryCount() == 0);
    FIC2_CHECK(gpu.DeviceRefs() == baseline);
}

FIC2_TEST(GpuPresentation_RecoveryFollowsPresentationDomain)
{
    // SRV across a target-only recreation: its device generation is unchanged.
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::ShaderResource, false, true) == PresentationRecovery::Keep);
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::ShaderResource, false, false) == PresentationRecovery::Keep);
    // Device change / renderer fallback: BGRA re-uploads, GPU-only content reloads.
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::ShaderResource, true, true) == PresentationRecovery::Reupload);
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::ShaderResource, true, false) == PresentationRecovery::Reload);
    // D2D bitmap on a new target.
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::Bitmap, true, true) == PresentationRecovery::Reupload);
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::Bitmap, false, true) == PresentationRecovery::Keep);
    FIC2_CHECK(DecidePresentationRecovery(PresentationKind::None, true, true) == PresentationRecovery::Keep);
}
