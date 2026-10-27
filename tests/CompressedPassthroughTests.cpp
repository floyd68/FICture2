#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/tests/WindowlessTestSupport.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserMainImage.h"
#include "ImageGpuResourceCache.h"
#include "MainImageTestSupport.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// The main view's GPU passthrough of block-compressed payloads (#53).
//  - CreateCompressedShaderResource on a WARP device: an sRGB-encoded BCn
//    payload becomes a _TYPELESS texture viewed as _UNORM (sampling returns the
//    stored values); other BCn formats keep their own format.
//  - Rendered through the real main image on a windowless backplate (D3D pass,
//    read back): an sRGB BC payload shows the same pixels as its _UNORM twin,
//    at first visit, on a GPU cache hit and in every channel-isolation mode,
//    while the presented and cached format stays the _SRGB source format.

using Microsoft::WRL::ComPtr;
using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::Frame;
using MainImageTestSupport::Bc1Color;
using MainImageTestSupport::ControlledDecodeService;
using MainImageTestSupport::ScopedDecodeService;
using MainImageTestSupport::SolidBc1;
using MainImageTestSupport::SolidBc7;

namespace
{
    uint32_t BlockBytes(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        default:
            return 16;
        }
    }

    // A w x h payload of `format` whose every block is `block` (zeros when empty).
    ImageAsyncBinding::Payload CompressedPayload(DXGI_FORMAT format, uint32_t w, uint32_t h,
        const std::vector<uint8_t>& block = {})
    {
        const uint32_t blockBytes = BlockBytes(format);
        const uint32_t blocksX = (w + 3) / 4;
        const uint32_t blocksY = (h + 3) / 4;
        ImageAsyncBinding::Payload payload {};
        payload.width = w;
        payload.height = h;
        payload.format = format;
        payload.rowPitch = blocksX * blockBytes;
        payload.sourceWasBlockCompressed = true;
        payload.blocks = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(blocksX) * blocksY * blockBytes, uint8_t(0));
        if (!block.empty())
        {
            for (size_t offset = 0; offset < payload.blocks->size(); offset += blockBytes)
            {
                std::copy(block.begin(), block.end(), payload.blocks->begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        return payload;
    }

    struct CreatedFormats
    {
        HRESULT hr { E_FAIL };
        DXGI_FORMAT texture { DXGI_FORMAT_UNKNOWN };
        DXGI_FORMAT view { DXGI_FORMAT_UNKNOWN };
    };

    CreatedFormats CreateAndInspect(ID3D11Device* device, DXGI_FORMAT format)
    {
        CreatedFormats out {};
        ComPtr<ID3D11ShaderResourceView> srv;
        out.hr = CreateCompressedShaderResource(device, CompressedPayload(format, 8, 8), srv);
        if (FAILED(out.hr) || !srv)
        {
            return out;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        srv->GetDesc(&sd);
        out.view = sd.Format;
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture)))
        {
            D3D11_TEXTURE2D_DESC td {};
            texture->GetDesc(&td);
            out.texture = td.Format;
        }
        return out;
    }

    ComPtr<ID3D11Device> WarpDevice()
    {
        ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        {
            return {};
        }
        return device;
    }

    // --- Main image on a windowless backplate --------------------------------

    constexpr UINT kSize = 64;   // logical = surface size (DPI scale 1)

    ImageCore::DecodedImage Decoded(const ImageAsyncBinding::Payload& payload)
    {
        ImageCore::DecodedImage out {};
        out.width = payload.width;
        out.height = payload.height;
        out.dxgiFormat = payload.format;
        out.sourceMipLevels = 1;
        out.sourceMipIndex = 0;
        out.sourceWasBlockCompressed = true;
        out.rowPitchBytes = payload.rowPitch;
        out.blocks = payload.blocks;
        out.blockBytes = static_cast<uint32_t>(payload.blocks->size());
        return out;
    }

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        ControlledDecodeService decodes;
        ScopedDecodeService decodeScope { &decodes };
        std::filesystem::path dir;
        FD2D::Backplate backplate { L"compressed-passthrough-test" };
        std::shared_ptr<ImageBrowserMainImage> main { std::make_shared<ImageBrowserMainImage>(L"main") };

        ~Scene()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        bool Create()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2PassthroughTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            if (FAILED(backplate.InitializeWindowless(kSize, kSize)))
            {
                return false;
            }
            main->SetLoadingSpinnerEnabled(false);
            return backplate.AddWnd(main) && backplate.D3DDevice() != nullptr;
        }

        // A real file, so the source identity (and with it the GPU cache) works.
        std::wstring File(const std::wstring& name)
        {
            const std::filesystem::path file = dir / name;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "dds";
            out.close();
            return CommonUtil::NormalizePath(file.wstring());
        }

        Frame RenderFrame()
        {
            backplate.Render();
            Frame frame;
            const HRESULT hr = frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)));
            FIC2_CHECK(SUCCEEDED(hr));
            return frame;
        }

        // Selects `path`; when that dispatches a decode, completes it with
        // `payload` (otherwise the GPU cache served it). Returns the frame that
        // shows it, after checking that the image is presented on the GPU.
        Frame Show(const std::wstring& path, const ImageAsyncBinding::Payload& payload, bool expectCacheHit)
        {
            const size_t before = decodes.requests.size();
            FIC2_CHECK(SUCCEEDED(main->SetSourceFile(path)));
            RenderFrame();
            FIC2_CHECK((decodes.requests.size() == before) == expectCacheHit);
            if (decodes.requests.size() > before)
            {
                FIC2_CHECK(decodes.requests.back().request.source == path);
                decodes.requests.back().callback(S_OK, Decoded(payload));
            }
            Frame frame = RenderFrame();
            frame = RenderFrame();
            const ImageLoadedInfo info = main->GetLoadedInfo();
            FIC2_CHECK(info.sourcePath == path);
            FIC2_CHECK(info.gpuPresentation);
            // The presented format is the source format, _SRGB included.
            FIC2_CHECK(info.format == payload.format);
            FIC2_CHECK(decodes.requests.size() == before + (expectCacheHit ? 0u : 1u));   // no CPU fallback
            return frame;
        }

        DXGI_FORMAT CachedFormat(const std::wstring& path)
        {
            ComPtr<ID3D11ShaderResourceView> srv;
            UINT w = 0;
            UINT h = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            ImageAlphaInfo alpha {};
            uint32_t levels = 0;
            uint32_t index = 0;
            const ImageSourceIdentity identity = ImageSourceIdentity::ForPath(path);
            if (!ImageGpuResourceCache::Instance().TryGet(path, 0, srv, w, h, format, alpha, levels, index, identity,
                backplate.GetGraphicsGeneration().device))
            {
                return DXGI_FORMAT_UNKNOWN;
            }
            return format;
        }
    };

    // The image (16 x 16 at zoom-to-fit) covers the whole 64 x 64 surface;
    // probes stay clear of its edges.
    constexpr POINT kProbes[] = { { 16, 16 }, { 32, 32 }, { 47, 20 }, { 20, 47 } };

    bool FrameIs(const Frame& frame, const Bgra& expected, const char* label)
    {
        bool ok = true;
        for (const POINT& p : kProbes)
        {
            const Bgra actual = frame.At(static_cast<UINT>(p.x), static_cast<UINT>(p.y));
            const auto close = [](uint8_t a, uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 2; };
            if (!close(actual.b, expected.b) || !close(actual.g, expected.g) || !close(actual.r, expected.r))
            {
                std::printf("    %s pixel (%ld, %ld): RGB %3u %3u %3u, expected %3u %3u %3u\n", label, p.x, p.y,
                    actual.r, actual.g, actual.b, expected.r, expected.g, expected.b);
                ok = false;
            }
        }
        return ok;
    }

    Bgra Grey(uint8_t v)
    {
        return Bgra { v, v, v, 255 };
    }

    // The expected look of `color` in channel mode `mode` (1..3: that channel
    // as grey, 4: alpha as grey; the test images are opaque).
    Bgra InChannelMode(const Bgra& color, int mode)
    {
        switch (mode)
        {
        case 1: return Grey(color.r);
        case 2: return Grey(color.g);
        case 3: return Grey(color.b);
        case 4: return Grey(255);
        default: return color;
        }
    }

    struct TwinCase
    {
        const char* name;
        DXGI_FORMAT unorm;
        DXGI_FORMAT srgb;
        std::vector<uint8_t> block;
        Bgra stored;   // the block-decoded value
    };

    void CheckTwins(const TwinCase& twin)
    {
        std::printf("  %s\n", twin.name);
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const auto unormPayload = CompressedPayload(twin.unorm, 16, 16, twin.block);
        const auto srgbPayload = CompressedPayload(twin.srgb, 16, 16, twin.block);
        const std::wstring unormPath = scene.File(L"unorm.dds");
        const std::wstring srgbPath = scene.File(L"srgb.dds");

        // First visit (decode), then a cache hit of each twin.
        for (const bool cacheHit : { false, true })
        {
            const Frame unormFrame = scene.Show(unormPath, unormPayload, cacheHit);
            FIC2_CHECK(FrameIs(unormFrame, twin.stored, cacheHit ? "UNORM (cache hit)" : "UNORM"));
            const Frame srgbFrame = scene.Show(srgbPath, srgbPayload, cacheHit);
            FIC2_CHECK(FrameIs(srgbFrame, twin.stored, cacheHit ? "SRGB (cache hit)" : "SRGB"));
            FIC2_CHECK(scene.CachedFormat(srgbPath) == twin.srgb);
            FIC2_CHECK(scene.CachedFormat(unormPath) == twin.unorm);
        }

        // Channel isolation of the sRGB texture shows the stored values.
        for (int mode = 1; mode <= 4; ++mode)
        {
            scene.main->SetChannelMode(mode);
            FIC2_CHECK(scene.main->ChannelMode() == mode);
            const Frame frame = scene.RenderFrame();
            FIC2_CHECK(FrameIs(frame, InChannelMode(twin.stored, mode), "SRGB channel mode"));
            scene.main->SetChannelMode(0);
        }
    }
}

FIC2_TEST(CompressedPassthrough_SrgbIsTypelessViewedAsUnorm)
{
    const ComPtr<ID3D11Device> device = WarpDevice();
    FIC2_REQUIRE(device);

    struct Expected
    {
        DXGI_FORMAT source;
        DXGI_FORMAT texture;
        DXGI_FORMAT view;
    };
    const Expected cases[] = {
        { DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM },
        { DXGI_FORMAT_BC2_UNORM_SRGB, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM },
        { DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM },
        { DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM },
    };
    for (const Expected& c : cases)
    {
        const CreatedFormats created = CreateAndInspect(device.Get(), c.source);
        if (FAILED(created.hr) || created.texture != c.texture || created.view != c.view)
        {
            std::printf("    format %d: hr 0x%08X texture %d view %d, expected texture %d view %d\n",
                static_cast<int>(c.source), static_cast<unsigned>(created.hr), static_cast<int>(created.texture),
                static_cast<int>(created.view), static_cast<int>(c.texture), static_cast<int>(c.view));
        }
        FIC2_CHECK(SUCCEEDED(created.hr));
        FIC2_CHECK(created.texture == c.texture);
        FIC2_CHECK(created.view == c.view);
    }
}

FIC2_TEST(CompressedPassthrough_LinearFormatsUnchanged)
{
    const ComPtr<ID3D11Device> device = WarpDevice();
    FIC2_REQUIRE(device);

    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC7_UNORM,
        DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM,
        DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16,
    };
    for (const DXGI_FORMAT format : formats)
    {
        const CreatedFormats created = CreateAndInspect(device.Get(), format);
        if (FAILED(created.hr) || created.texture != format || created.view != format)
        {
            std::printf("    format %d: hr 0x%08X texture %d view %d\n", static_cast<int>(format),
                static_cast<unsigned>(created.hr), static_cast<int>(created.texture), static_cast<int>(created.view));
        }
        FIC2_CHECK(SUCCEEDED(created.hr));
        FIC2_CHECK(created.texture == format);
        FIC2_CHECK(created.view == format);
    }
}

FIC2_TEST(CompressedPassthrough_RejectsMissingDeviceOrNonBcPayload)
{
    ComPtr<ID3D11ShaderResourceView> srv;
    FIC2_CHECK(CreateCompressedShaderResource(nullptr, CompressedPayload(DXGI_FORMAT_BC1_UNORM_SRGB, 8, 8), srv) == E_INVALIDARG);
    FIC2_CHECK(!srv);

    const ComPtr<ID3D11Device> device = WarpDevice();
    FIC2_REQUIRE(device);
    ImageAsyncBinding::Payload bgra {};
    bgra.width = 4;
    bgra.height = 4;
    bgra.rowPitch = 16;
    bgra.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bgra.blocks = std::make_shared<std::vector<uint8_t>>(64, uint8_t(0));
    FIC2_CHECK(CreateCompressedShaderResource(device.Get(), bgra, srv) == E_INVALIDARG);
    FIC2_CHECK(!srv);
}

// The decoded block values of each sRGB payload are displayed unchanged (the
// same pixels as its _UNORM twin) through the main view's D3D path.
FIC2_TEST(CompressedPassthrough_SrgbRendersStoredValues)
{
    // 5:6:5 (16, 32, 16): RGB 132, 130, 132 (linearized it would read ~59).
    const uint16_t bc1Grey = static_cast<uint16_t>((16 << 11) | (32 << 5) | 16);
    // 5:6:5 (8, 32, 24): RGB 66, 130, 198.
    const uint16_t bc1Color = static_cast<uint16_t>((8 << 11) | (32 << 5) | 24);
    const TwinCase cases[] = {
        { "BC1 grey", DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, SolidBc1(bc1Grey), Bc1Color(bc1Grey) },
        { "BC1 color", DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, SolidBc1(bc1Color), Bc1Color(bc1Color) },
        { "BC7 grey", DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, SolidBc7(0x80, 0x80, 0x80), Grey(0x80) },
        { "BC7 color", DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, SolidBc7(0x40, 0x80, 0xC0),
            Bgra { 0xC0, 0x80, 0x40, 255 } },
    };
    for (const TwinCase& twin : cases)
    {
        CheckTwins(twin);
    }
}
