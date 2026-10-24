#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserMainImage.h"
#include "ImageGpuResourceCache.h"
#include "ImageSourceIdentity.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// ImageBrowserMainImage presentation paths on a windowless backplate: GPU
// content comes from ImageGpuResourceCache (WARP SRVs), decodes go to a fake
// service that never completes, and the backplate's device generation changes
// through FD2D's test access - the same way a real device loss changes it.

namespace FD2D
{
    struct BackplateTestAccess
    {
        static void DeviceLost(Backplate& backplate)
        {
            backplate.DiscardDeviceResources();
            backplate.InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        }
    };
}

using Microsoft::WRL::ComPtr;

namespace
{
    class IdleDecodeService : public IImageDecodeService
    {
    public:
        ImageCore::ImageHandle RequestDecoded(const ImageCore::ImageRequest&, ImageCore::DecodedImageLoadCallback) override
        {
            return ++m_next;
        }
        void Cancel(ImageCore::ImageHandle) override {}

    private:
        ImageCore::ImageHandle m_next { 0 };
    };

    struct ScopedIdleDecodes
    {
        IdleDecodeService service;
        ScopedIdleDecodes() { ImageAsyncBinding::SetDecodeServiceForTesting(&service); }
        ~ScopedIdleDecodes() { ImageAsyncBinding::SetDecodeServiceForTesting(nullptr); }
    };

    struct ComScope
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
    };

    void WriteBytes(const std::filesystem::path& path, size_t count)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::string data(count, 'x');
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    // A DDS-like file on disk whose mips 0..2 (16, 8, 4 px) are in the GPU cache.
    struct CachedMipFile
    {
        ComScope com;
        ScopedIdleDecodes decodes;
        ComPtr<ID3D11Device> device;
        std::filesystem::path dir;
        std::filesystem::path file;
        std::wstring key;   // normalized path, as the main image uses it

        bool Create(uint64_t deviceGeneration)
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2MainImageTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);
            file = dir / L"texture.dds";
            WriteBytes(file, 256);
            key = CommonUtil::NormalizePath(file.wstring());

            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            {
                return false;
            }
            ImageGpuResourceCache::Instance().Clear();
            const ImageSourceIdentity identity = ImageSourceIdentity::ForPath(key);
            for (uint32_t mip = 0; mip < 3; ++mip)
            {
                const UINT size = 16u >> mip;
                ComPtr<ID3D11ShaderResourceView> srv;
                if (!MakeSrv(size, srv))
                {
                    return false;
                }
                ImageGpuResourceCache::Instance().Put(key, mip, srv, size, size, DXGI_FORMAT_B8G8R8A8_UNORM,
                    ImageAlphaInfo {}, 3, mip, identity, deviceGeneration);
            }
            return ImageGpuResourceCache::Instance().EntryCount() == 3;
        }

        bool MakeSrv(UINT size, ComPtr<ID3D11ShaderResourceView>& out) const
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = size;
            desc.Height = size;
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

        ~CachedMipFile()
        {
            ImageGpuResourceCache::Instance().Clear();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
}

FIC2_TEST(MainImage_MipSelectionRejectsCacheAfterSourceReplaced)
{
    FD2D::Backplate backplate(L"main-image-test");
    CachedMipFile cached;
    FIC2_REQUIRE(cached.Create(backplate.GetGraphicsGeneration().device));

    auto main = std::make_shared<ImageBrowserMainImage>(L"main");
    FIC2_REQUIRE(backplate.AddWnd(main));
    FIC2_REQUIRE(SUCCEEDED(main->SetSourceFile(cached.file.wstring())));
    FIC2_REQUIRE(main->GetLoadedInfo().width == 16);   // mip 0 from the cache
    FIC2_REQUIRE(main->MipLevels() == 3);

    // Cached mips work while the file is unchanged.
    FIC2_CHECK(main->SelectMip(2));
    FIC2_CHECK(main->GetLoadedInfo().width == 4);
    FIC2_CHECK(main->SelectMip(0));
    FIC2_CHECK(main->GetLoadedInfo().width == 16);

    // The file is replaced while it stays selected; then another mip is chosen.
    WriteBytes(cached.file, 512);
    FIC2_CHECK(main->SelectMip(1));
    const ImageLoadedInfo info = main->GetLoadedInfo();
    FIC2_CHECK(info.width != 8);              // not the stale cached mip 1
    FIC2_CHECK(info.sourcePath.empty());      // reloading the new content
    FIC2_CHECK(ImageGpuResourceCache::Instance().EntryCount() == 0);
}

FIC2_TEST(MainImage_DeviceLossAtNonzeroMipKeepsSourceMetadata)
{
    FD2D::Backplate backplate(L"main-image-test");
    CachedMipFile cached;
    FIC2_REQUIRE(cached.Create(backplate.GetGraphicsGeneration().device));

    auto main = std::make_shared<ImageBrowserMainImage>(L"main");
    FIC2_REQUIRE(backplate.AddWnd(main));
    FIC2_REQUIRE(SUCCEEDED(main->SetSourceFile(cached.file.wstring())));
    FIC2_REQUIRE(main->SelectMip(2));
    {
        const ImageLoadedInfo info = main->GetLoadedInfo();
        FIC2_REQUIRE(info.width == 4);
        FIC2_REQUIRE(info.sourceWidth == 16);
    }

    // The GPU-only SRV is lost with the device: reload, keeping what is known
    // about the source.
    FD2D::BackplateTestAccess::DeviceLost(backplate);
    const ImageLoadedInfo info = main->GetLoadedInfo();
    FIC2_CHECK(info.width == 0);
    FIC2_CHECK(info.sourcePath.empty());
    FIC2_CHECK(info.sourceWidth == 16);
    FIC2_CHECK(info.sourceHeight == 16);
    FIC2_CHECK(main->MipLevels() == 3);
    // Old-device SRVs were released from the cache.
    FIC2_CHECK(ImageGpuResourceCache::Instance().EntryCount() == 0);
}
