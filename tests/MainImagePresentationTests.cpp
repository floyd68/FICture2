#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/Core.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserMainImage.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageGpuResourceCache.h"
#include "ImageSourceIdentity.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"

#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
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
        static void TargetRecreated(Backplate& backplate)
        {
            backplate.InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
        }

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
        ImageCore::ImageHandle RequestDecoded(const ImageCore::ImageRequest&, ImageCore::DecodedImageLoadCallback,
            ImageCore::RequestPriority) override
        {
            return ++m_next;
        }
        void Cancel(ImageCore::ImageHandle) override {}
        void SetPriority(ImageCore::ImageHandle, ImageCore::RequestPriority) override {}

    private:
        ImageCore::ImageHandle m_next { 0 };
    };

    // Records requests (with their mip) and completes them on demand.
    class RecordingDecodeService : public IImageDecodeService
    {
    public:
        struct Request
        {
            ImageCore::ImageRequest request {};
            ImageCore::DecodedImageLoadCallback callback {};
        };

        ImageCore::ImageHandle RequestDecoded(const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback, ImageCore::RequestPriority) override
        {
            requests.push_back({ request, std::move(callback) });
            return static_cast<ImageCore::ImageHandle>(requests.size());
        }
        void Cancel(ImageCore::ImageHandle) override {}
        void SetPriority(ImageCore::ImageHandle, ImageCore::RequestPriority) override {}

        // Completes the latest request with a BGRA image of the given mip.
        void CompleteLatest(uint32_t size, uint32_t mipLevels, uint32_t mipIndex)
        {
            ImageCore::DecodedImage image {};
            image.width = size;
            image.height = size;
            image.rowPitchBytes = size * 4u;
            image.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            image.blocks = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size) * size * 4u, uint8_t(0x80));
            image.blockBytes = static_cast<uint32_t>(image.blocks->size());
            image.sourceMipLevels = mipLevels;
            image.sourceMipIndex = mipIndex;
            requests.back().callback(S_OK, std::move(image));
        }

        std::vector<Request> requests;
    };

    // Windowless render target for OnRender (no D3D device: CPU payloads are
    // presented as D2D bitmaps).
    struct WicTarget
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create()
        {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
                FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        void Render(FD2D::Wnd& wnd)
        {
            target->BeginDraw();
            wnd.OnRender(target.Get());
            (void)target->EndDraw();
        }
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

FIC2_TEST(MainImage_MipSelectionAfterSourceReplacedReloadsMip0ThenRequestedMip)
{
    FD2D::Backplate backplate(L"main-image-test");
    CachedMipFile cached;
    FIC2_REQUIRE(cached.Create(backplate.GetGraphicsGeneration().device));
    // Decodes go to a recording service instead of the idle one.
    RecordingDecodeService decodes;
    ImageAsyncBinding::SetDecodeServiceForTesting(&decodes);
    WicTarget canvas;
    FIC2_REQUIRE(canvas.Create());

    auto main = std::make_shared<ImageBrowserMainImage>(L"main");
    FIC2_REQUIRE(backplate.AddWnd(main));
    main->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 64.0f, 64.0f));
    FIC2_REQUIRE(SUCCEEDED(main->SetSourceFile(cached.file.wstring())));
    FIC2_REQUIRE(main->GetLoadedInfo().width == 16);   // old mip 0 from the cache
    FIC2_CHECK(main->SelectMip(2));
    FIC2_CHECK(main->GetLoadedInfo().width == 4);      // cached mips work while unchanged
    FIC2_CHECK(main->SelectMip(0));

    // The file is replaced (a new 32 px image with 4 mips) while it stays
    // selected; then mip 1 is chosen.
    WriteBytes(cached.file, 512);
    FIC2_CHECK(main->SelectMip(1));
    FIC2_CHECK(main->GetLoadedInfo().width != 8);      // not the stale cached mip 1
    FIC2_CHECK(ImageGpuResourceCache::Instance().EntryCount() == 0);

    // The first decode after the replacement is mip 0 (it supplies the new size).
    canvas.Render(*main);
    FIC2_REQUIRE(decodes.requests.size() == 1);
    FIC2_CHECK(decodes.requests.back().request.mipLevel == 0);
    decodes.CompleteLatest(32, 4, 0);
    canvas.Render(*main);
    {
        const ImageLoadedInfo info = main->GetLoadedInfo();
        FIC2_CHECK(info.width == 32);
        FIC2_CHECK(info.sourceWidth == 32);
        FIC2_CHECK(main->MipLevels() == 4);
    }

    // Presenting mip 0 continues to the requested mip.
    FIC2_REQUIRE(decodes.requests.size() == 2);
    FIC2_CHECK(decodes.requests.back().request.mipLevel == 1);
    decodes.CompleteLatest(16, 4, 1);
    canvas.Render(*main);
    const ImageLoadedInfo info = main->GetLoadedInfo();
    FIC2_CHECK(info.width == 16);
    FIC2_CHECK(info.sourceMipIndex == 1);
    FIC2_CHECK(info.sourceWidth == 32);   // the new source's full size
    FIC2_CHECK(info.sourceHeight == 32);
    FIC2_CHECK(!info.sourcePath.empty());

    main.reset();
    ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
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

namespace
{
    // A software render target on its own D2D factory (= its own resource domain).
    struct DomainCanvas
    {
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create(IWICImagingFactory* wic)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(96, 96, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        HRESULT Draw(FD2D::Wnd& wnd)
        {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 1));
            wnd.OnRender(target.Get());
            return target->EndDraw();
        }
    };

    struct CoreScope
    {
        HRESULT hr = S_FALSE;
        CoreScope()
        {
            FD2D::InitContext context {};
            context.instance = GetModuleHandleW(nullptr);
            hr = FD2D::Core::Initialize(context);
        }
        ~CoreScope()
        {
            if (hr == S_OK)
            {
                FD2D::Core::Shutdown();
            }
        }
    };
}

FIC2_TEST(MainImage_ThumbnailTilesRebuildBrushesAfterTargetChange)
{
    // Regression (device recreation left the window frozen): the tiles' brushes
    // and their embedded caption/badge Text labels kept resources from the old
    // target, so every frame after a device loss failed EndDraw.
    ComScope com;
    CoreScope core;   // captions need DirectWrite
    ComPtr<IWICImagingFactory> wic;
    FIC2_REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    DomainCanvas a;
    DomainCanvas b;
    FIC2_REQUIRE(a.Create(wic.Get()));
    FIC2_REQUIRE(b.Create(wic.Get()));

    struct Case
    {
        const char* name;
        std::shared_ptr<FD2D::Wnd> wnd;
    };
    auto imageTile = std::make_shared<ThumbImageTile>(L"imageTile");
    imageTile->SetCaption(L"caption.dds");
    auto navTile = std::make_shared<ThumbNavTile>(L"navTile");
    navTile->SetIcon(ThumbNavTile::IconKind::Folder);
    navTile->SetText(L"folder");
    navTile->SetBadgeText(L"ZIP");
    const Case cases[] = { { "ThumbImageTile", imageTile }, { "ThumbNavTile", navTile } };

    for (const auto& c : cases)
    {
        FD2D::Backplate backplate(L"tile-test");
        FIC2_REQUIRE(backplate.AddWnd(c.wnd));
        c.wnd->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 96.0f, 96.0f));

        const HRESULT first = a.Draw(*c.wnd);
        const HRESULT otherDomainSameGeneration = b.Draw(*c.wnd);
        FD2D::BackplateTestAccess::TargetRecreated(backplate);
        const HRESULT afterTargetBump = b.Draw(*c.wnd);
        std::printf("  %-14s first 0x%08X, other domain same generation 0x%08X, after target bump 0x%08X\n",
            c.name, static_cast<unsigned>(first), static_cast<unsigned>(otherDomainSameGeneration),
            static_cast<unsigned>(afterTargetBump));
        FIC2_CHECK(SUCCEEDED(first));
        FIC2_CHECK(FAILED(otherDomainSameGeneration));   // resources are cached ...
        FIC2_CHECK(SUCCEEDED(afterTargetBump));          // ... and rebuilt for the new target
    }
}

FIC2_TEST(MainImage_ThumbnailRestoresFromCachedPayloadAfterGraphicsChanges)
{
    // Target recreation and device loss rebuild a thumbnail's D2D bitmap from
    // its pinned CPU payload (#6) without a new decode, and keep the cache
    // entries: CPU payloads do not depend on graphics generations.
    ComScope com;
    ComPtr<IWICImagingFactory> wic;
    FIC2_REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    DomainCanvas a;
    DomainCanvas b;
    FIC2_REQUIRE(a.Create(wic.Get()));
    FIC2_REQUIRE(b.Create(wic.Get()));

    RecordingDecodeService decodes;
    ImageAsyncBinding::SetDecodeServiceForTesting(&decodes);
    {
        const std::wstring path = CommonUtil::NormalizePath(L"C:\\FICture2Tests\\Thumb\\thumb.png");
        ImageSourceIdentity identity {};
        identity.size = 1234;
        identity.writeTime = 5678;
        identity.known = true;
        auto cache = std::make_shared<ThumbPayloadCache>();

        FD2D::Backplate backplate(L"thumb-recovery-test");
        auto thumb = std::make_shared<ImageBrowserThumbImage>(L"thumb");
        FIC2_REQUIRE(backplate.AddWnd(thumb));
        thumb->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 96.0f, 96.0f));
        thumb->SetThumbnailSize({ 64.0f, 64.0f });
        thumb->SetPayloadCache(cache);
        FIC2_REQUIRE(SUCCEEDED(thumb->SetSourceFile(path, identity)));
        FIC2_REQUIRE(decodes.requests.size() == 1);
        decodes.CompleteLatest(8, 1, 0);
        FIC2_CHECK(SUCCEEDED(a.Draw(*thumb)));
        FIC2_CHECK(thumb->GetBitmapSize().width == 8.0f);
        FIC2_CHECK(cache->EntryCount() == 1);
        FIC2_CHECK(cache->GetStats().pinnedEntries == 1);

        const auto cachedAfter = [&](const char* step)
        {
            const HRESULT hr = (std::string(step) == "target") ? b.Draw(*thumb) : a.Draw(*thumb);
            std::printf("  after %s change: draw 0x%08X, bitmap %.0f px, %zu requests, %zu entries\n",
                step, static_cast<unsigned>(hr), thumb->GetBitmapSize().width, decodes.requests.size(),
                cache->EntryCount());
            FIC2_CHECK(SUCCEEDED(hr));
            FIC2_CHECK(thumb->GetBitmapSize().width == 8.0f);
            FIC2_CHECK(decodes.requests.size() == 1);   // no new decode
            FIC2_CHECK(cache->EntryCount() == 1);       // no entry dropped
            FIC2_CHECK(cache->GetStats().pinnedEntries == 1);
            FIC2_CHECK(cache->Find(path, identity, ThumbDecodeOptions {}).size() == 1);
        };

        // The bitmap belongs to the old target (domain a): drawing it into
        // domain b works only once it was rebuilt from the CPU payload.
        FD2D::BackplateTestAccess::TargetRecreated(backplate);
        cachedAfter("target");
        FD2D::BackplateTestAccess::DeviceLost(backplate);
        cachedAfter("device");
    }
    ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
}
