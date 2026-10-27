#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/tests/WindowlessTestSupport.h"
#include "ImageBrowserMainImage.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "MainImageTestSupport.h"

#include <DirectXTex.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

// sRGB main-view presentation, end to end (#54).
//
// Each fixture is a DDS twin pair with identical texel (or block) bytes, one
// declared _SRGB and one _UNORM, written to a temp folder. The test opens it in
// a real ImageBrowserMainImage on a windowless D3D backplate, completes the
// main image's own decode request through ImageCore's real decode dispatcher
// (so ImageCore's BGRA8 conversion and BCn passthrough are what is tested),
// renders through the real D3D-then-D2D order and reads the frame back.
//
// Paths (checked through ImageLoadedInfo and the decode requests):
//  - GpuUncompressed: R8G8B8A8 / B8G8R8A8 decoded to CPU BGRA8 by ImageCore,
//    uploaded as a B8G8R8A8 SRV and drawn in the D3D pass.
//  - GpuPassthrough:  BCn blocks passed through by ImageCore and drawn from a
//    compressed SRV in the D3D pass.
//  - CpuFallback:     the main image's first (GPU-allowed) decode delivers a
//    compressed payload the device rejects (a zero-size texture), which is
//    what forces the CPU decode; the re-request (allowGpuCompressedDDS off) is
//    decoded to BGRA8 by ImageCore and drawn as a D2D bitmap in the D2D pass.
//
// Oracles (64 x 64 image on a 64 x 64 surface: 1:1, no zoom/pan/rotation,
// point sampling, RGBA channel mode; interior pixels only):
//  - Opaque: both twins read back the stored (block-decoded) values, within
//    +/-1, and match each other within +/-1.
//  - Translucent: partial alpha over a known opaque clear color, a transparent
//    clear and the alpha checkerboard (fixed phase: the image starts at the
//    surface origin). Both twins match each other within +/-1 and match an
//    independently computed composition (ImageAlphaPresentation's rules:
//    straight coverage is premultiplied, premultiplied coverage is kept, data
//    alpha shows opaque straight RGB; then premultiplied source-over onto the
//    premultiplied target), output alpha included.

using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::Frame;
using MainImageTestSupport::Bc1Color;
using MainImageTestSupport::ControlledDecodeService;
using MainImageTestSupport::ScopedDecodeService;
using MainImageTestSupport::SolidBc1;
using MainImageTestSupport::SolidBc7;

namespace
{
    constexpr UINT kSize = 64;      // image = surface size, so the view is 1:1
    constexpr UINT kBlocks = kSize / 4;
    constexpr UINT kMargin = 2;     // probes stay clear of the image edges
    constexpr int kTolerance = 1;

    enum class RenderPath
    {
        GpuUncompressed,
        GpuPassthrough,
        CpuFallback,
    };

    const char* PathName(RenderPath path)
    {
        switch (path)
        {
        case RenderPath::GpuUncompressed: return "GPU uncompressed (BGRA8 SRV)";
        case RenderPath::GpuPassthrough: return "GPU BCn passthrough";
        default: return "CPU fallback (D2D bitmap)";
        }
    }

    // --- Fixtures --------------------------------------------------------------

    // Identical bytes declared as `unorm` and `srgb`, and the values every pixel
    // decodes to (straight, or premultiplied when `alphaMode` says so).
    struct Fixture
    {
        const char* name { "" };
        DXGI_FORMAT unorm { DXGI_FORMAT_UNKNOWN };
        DXGI_FORMAT srgb { DXGI_FORMAT_UNKNOWN };
        DirectX::TEX_ALPHA_MODE alphaMode { DirectX::TEX_ALPHA_MODE_UNKNOWN };
        std::vector<uint8_t> bytes;    // texels or blocks, tightly packed
        std::vector<Bgra> decoded;     // kSize * kSize, row-major
        bool translucent { false };
    };

    // Mid-tones (where sRGB-to-linear differs most, e.g. 0x80 -> 0x37), every
    // pixel different. Translucent alpha spans 0..255 (x*4 + y*3 mod 256), so
    // most pixels are partial.
    Bgra TexelColor(UINT x, UINT y, bool translucent)
    {
        Bgra c {};
        c.r = static_cast<uint8_t>(0x18 + 3 * x);
        c.g = static_cast<uint8_t>(0x20 + 3 * y);
        c.b = static_cast<uint8_t>(0x40 + (x * 5 + y * 3) % 0x80);
        c.a = translucent ? static_cast<uint8_t>((x * 4 + y * 3) % 256) : uint8_t(255);
        return c;
    }

    uint8_t Premultiply(uint8_t c, uint8_t a)
    {
        return static_cast<uint8_t>((static_cast<unsigned>(c) * a + 127u) / 255u);
    }

    Fixture Uncompressed(const char* name, bool bgraOrder, bool translucent, bool premultiplied)
    {
        Fixture f;
        f.name = name;
        f.unorm = bgraOrder ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        f.srgb = bgraOrder ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        f.alphaMode = premultiplied ? DirectX::TEX_ALPHA_MODE_PREMULTIPLIED : DirectX::TEX_ALPHA_MODE_UNKNOWN;
        f.translucent = translucent;
        for (UINT y = 0; y < kSize; ++y)
        {
            for (UINT x = 0; x < kSize; ++x)
            {
                Bgra c = TexelColor(x, y, translucent);
                if (premultiplied)
                {
                    c.r = Premultiply(c.r, c.a);
                    c.g = Premultiply(c.g, c.a);
                    c.b = Premultiply(c.b, c.a);
                }
                f.decoded.push_back(c);
                const uint8_t texel[4] = { bgraOrder ? c.b : c.r, c.g, bgraOrder ? c.r : c.b, c.a };
                f.bytes.insert(f.bytes.end(), texel, texel + 4);
            }
        }
        return f;
    }

    // One solid block per 4 x 4 tile, a different color in each tile.
    template <typename BlockOf>
    Fixture BlockFixture(const char* name, DXGI_FORMAT unorm, DXGI_FORMAT srgb, bool translucent, BlockOf blockOf)
    {
        Fixture f;
        f.name = name;
        f.unorm = unorm;
        f.srgb = srgb;
        f.translucent = translucent;
        f.decoded.resize(static_cast<size_t>(kSize) * kSize);
        for (UINT by = 0; by < kBlocks; ++by)
        {
            for (UINT bx = 0; bx < kBlocks; ++bx)
            {
                Bgra value {};
                const std::vector<uint8_t> block = blockOf(bx, by, value);
                f.bytes.insert(f.bytes.end(), block.begin(), block.end());
                for (UINT y = 0; y < 4; ++y)
                {
                    for (UINT x = 0; x < 4; ++x)
                    {
                        f.decoded[static_cast<size_t>(by * 4 + y) * kSize + bx * 4 + x] = value;
                    }
                }
            }
        }
        return f;
    }

    Fixture Bc1Opaque()
    {
        return BlockFixture("BC1 opaque", DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, false,
            [](UINT bx, UINT by, Bgra& value)
            {
                const uint16_t r5 = static_cast<uint16_t>((6 + bx) & 0x1F);
                const uint16_t g6 = static_cast<uint16_t>((12 + by * 2) & 0x3F);
                const uint16_t b5 = static_cast<uint16_t>((8 + bx + by) & 0x1F);
                const uint16_t rgb565 = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
                value = Bc1Color(rgb565);
                return SolidBc1(rgb565);
            });
    }

    // Every channel value is even and in [2, 254] (exact in SolidBc7); opaque
    // alpha is 255.
    Fixture Bc7(bool translucent)
    {
        return BlockFixture(translucent ? "BC7 translucent" : "BC7 opaque", DXGI_FORMAT_BC7_UNORM,
            DXGI_FORMAT_BC7_UNORM_SRGB, translucent,
            [translucent](UINT bx, UINT by, Bgra& value)
            {
                value.r = static_cast<uint8_t>(0x20 + bx * 12);
                value.g = static_cast<uint8_t>(0x30 + by * 10);
                value.b = static_cast<uint8_t>(0x40 + ((bx * 3 + by * 5) % 16) * 8);
                value.a = translucent ? static_cast<uint8_t>(2 + ((bx * 16 + by * 5) % 127) * 2) : uint8_t(255);
                return SolidBc7(value.r, value.g, value.b, value.a);
            });
    }

    std::vector<uint8_t> DdsBytes(const Fixture& fixture, DXGI_FORMAT format)
    {
        DirectX::Image image {};
        image.width = kSize;
        image.height = kSize;
        image.format = format;
        FIC2_REQUIRE(SUCCEEDED(DirectX::ComputePitch(format, kSize, kSize, image.rowPitch, image.slicePitch)));
        FIC2_REQUIRE(fixture.bytes.size() == image.slicePitch);
        image.pixels = const_cast<uint8_t*>(fixture.bytes.data());

        DirectX::TexMetadata metadata {};
        metadata.width = kSize;
        metadata.height = kSize;
        metadata.depth = 1;
        metadata.arraySize = 1;
        metadata.mipLevels = 1;
        metadata.format = format;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
        metadata.SetAlphaMode(fixture.alphaMode);

        // Both twins get a DX10 header that records the alpha mode, so they
        // declare the same one.
        DirectX::Blob blob;
        FIC2_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(&image, 1, metadata,
            DirectX::DDS_FLAGS_FORCE_DX10_EXT | DirectX::DDS_FLAGS_FORCE_DX10_EXT_MISC2, blob)));
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    // --- Decoding through ImageCore -------------------------------------------------

    // Decodes `dds` for the main image's own request, as a decode thread would.
    ImageCore::PipelineResult DecodeWithImageCore(const ImageCore::ImageRequest& request, const std::vector<uint8_t>& dds)
    {
        // DirectXTex conversions may go through WIC, and DirectXTex caches its
        // WIC factory process-wide. Keeping the MTA alive for the rest of the
        // process keeps that factory valid after each scene's COM scope ends
        // (without tying the main thread to an apartment).
        static const HRESULT s_hrMta = []
        {
            CO_MTA_USAGE_COOKIE cookie {};
            return CoIncrementMTAUsage(&cookie);
        }();
        FIC2_REQUIRE(SUCCEEDED(s_hrMta));
        ImageCore::RegisterBuiltInDecoders();
        ImageCore::DecodeInput input {};
        input.bytes = std::span<const uint8_t>(dds.data(), dds.size());
        input.header = std::span<const uint8_t>(dds.data(), (std::min)(dds.size(), size_t { 512 }));
        ImageCore::ImageDecodeDispatcher dispatcher;
        return dispatcher.Decode(request, nullptr, input);
    }

    // A compressed payload the device cannot create a texture for (zero size):
    // the GPU passthrough fails, which is what forces the CPU decode.
    ImageCore::DecodedImage RejectedPassthrough()
    {
        ImageCore::DecodedImage out {};
        out.width = 0;
        out.height = 0;
        out.dxgiFormat = DXGI_FORMAT_BC1_UNORM;
        out.sourceWasBlockCompressed = true;
        out.rowPitchBytes = 8;
        out.blocks = std::make_shared<std::vector<uint8_t>>(8, uint8_t(0));
        out.blockBytes = 8;
        return out;
    }

    // --- Scene -------------------------------------------------------------------

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        ControlledDecodeService decodes;
        ScopedDecodeService decodeScope { &decodes };
        std::filesystem::path dir;
        FD2D::Backplate backplate { L"srgb-readback-test" };
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
                (L"FICture2SrgbReadbackTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            if (FAILED(backplate.InitializeWindowless(kSize, kSize)))
            {
                return false;
            }
            main->SetLoadingSpinnerEnabled(false);
            // Point sampling: no filtering in either pass.
            main->SetHighQualitySampling(false);
            return backplate.AddWnd(main) && backplate.D3DDevice() != nullptr;
        }

        std::wstring WriteFile(const std::wstring& name, const std::vector<uint8_t>& bytes)
        {
            const std::filesystem::path file = dir / name;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            return CommonUtil::NormalizePath(file.wstring());
        }

        Frame RenderFrame()
        {
            backplate.Render();
            Frame frame;
            const HRESULT hr = frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)));
            FIC2_REQUIRE(SUCCEEDED(hr));
            FIC2_REQUIRE(frame.width == kSize && frame.height == kSize);
            return frame;
        }

        // Selects `path`, completes the main image's decode request(s) through
        // ImageCore along `renderPath`, and checks that the image is presented
        // that way.
        void Show(const std::wstring& path, const std::vector<uint8_t>& dds, DXGI_FORMAT sourceFormat, RenderPath renderPath)
        {
            const size_t before = decodes.requests.size();
            FIC2_REQUIRE(SUCCEEDED(main->SetSourceFile(path)));
            RenderFrame();
            FIC2_REQUIRE(decodes.requests.size() == before + 1);
            FIC2_CHECK(decodes.requests.back().request.source == path);
            // The main image on a D3D backplate always asks for BCn passthrough first.
            FIC2_CHECK(decodes.requests.back().request.allowGpuCompressedDDS);

            if (renderPath == RenderPath::CpuFallback)
            {
                const auto callback = decodes.requests.back().callback;
                callback(S_OK, RejectedPassthrough());
                RenderFrame();
                FIC2_REQUIRE(decodes.requests.size() == before + 2);
                FIC2_CHECK(decodes.requests.back().request.source == path);
                FIC2_CHECK(!decodes.requests.back().request.allowGpuCompressedDDS);
            }

            const ImageCore::ImageRequest request = decodes.requests.back().request;
            const auto callback = decodes.requests.back().callback;
            ImageCore::PipelineResult result = DecodeWithImageCore(request, dds);
            if (FAILED(result.hr))
            {
                std::printf("    ImageCore decode failed: 0x%08X\n", static_cast<unsigned>(result.hr));
            }
            FIC2_REQUIRE(SUCCEEDED(result.hr));
            const DXGI_FORMAT presentedFormat =
                renderPath == RenderPath::GpuPassthrough ? sourceFormat : DXGI_FORMAT_B8G8R8A8_UNORM;
            // ImageCore's output decides the path: BCn blocks or CPU BGRA8.
            FIC2_REQUIRE(result.image.dxgiFormat == presentedFormat);
            callback(result.hr, std::move(result.image));

            RenderFrame();
            RenderFrame();
            const ImageLoadedInfo info = main->GetLoadedInfo();
            FIC2_CHECK(info.sourcePath == path);
            FIC2_CHECK(info.width == kSize && info.height == kSize);
            FIC2_CHECK(info.format == presentedFormat);
            FIC2_CHECK(info.gpuPresentation == (renderPath != RenderPath::CpuFallback));
            FIC2_CHECK(!info.highQualitySampling);
            FIC2_CHECK(info.zoomScale == 1.0f && info.panX == 0.0f && info.panY == 0.0f && info.rotationQuarters == 0);
            FIC2_CHECK(info.channelMode == 0);
            // No further decode (and no fallback on the GPU paths).
            FIC2_CHECK(decodes.requests.size() == before + (renderPath == RenderPath::CpuFallback ? 2u : 1u));
        }
    };

    // --- Composition oracle ------------------------------------------------------

    enum class Background
    {
        OpaqueColor,
        Transparent,
        Checkerboard,
    };

    const char* BackgroundName(Background background)
    {
        switch (background)
        {
        case Background::OpaqueColor: return "opaque clear color";
        case Background::Transparent: return "transparent clear";
        default: return "checkerboard";
        }
    }

    // Opaque clear color (premultiplied target, alpha 1).
    constexpr Bgra kClear { 0x90, 0x60, 0x30, 255 };

    // The 8-pixel checkerboard (light F0, dark 70) starts at the image origin,
    // which is the surface origin here.
    Bgra BackgroundAt(Background background, UINT x, UINT y)
    {
        switch (background)
        {
        case Background::OpaqueColor:
            return kClear;
        case Background::Transparent:
            return Bgra { 0, 0, 0, 0 };
        default:
        {
            const bool dark = (((x / 8) + (y / 8)) & 1u) != 0;
            const uint8_t v = dark ? uint8_t(0x70) : uint8_t(0xF0);
            return Bgra { v, v, v, 255 };
        }
        }
    }

    // The expected composed pixel, from the presentation rules
    // (ImageAlphaPresentation / the D3D image shader): data alpha shows the
    // straight color opaque; coverage alpha premultiplies a straight color and
    // keeps a premultiplied one; the result goes source-over (ONE,
    // INV_SRC_ALPHA) onto the premultiplied background, alpha included.
    Bgra Compose(const Bgra& texel, bool premultiplied, ImageCore::AlphaUsage usage, const Bgra& background)
    {
        const double a = texel.a / 255.0;
        const auto channel = [&](uint8_t c, uint8_t dst) -> uint8_t
        {
            double src = 0.0;
            if (usage == ImageCore::AlphaUsage::Data)
            {
                src = (premultiplied && texel.a != 0) ? (std::min)(255.0, c / a) : c;
                return static_cast<uint8_t>(std::lround(src));
            }
            src = premultiplied ? c : c * a;
            return static_cast<uint8_t>(std::lround((std::min)(255.0, src + dst * (1.0 - a))));
        };
        Bgra out {};
        out.b = channel(texel.b, background.b);
        out.g = channel(texel.g, background.g);
        out.r = channel(texel.r, background.r);
        out.a = usage == ImageCore::AlphaUsage::Data
            ? uint8_t(255)
            : static_cast<uint8_t>(std::lround(texel.a + background.a * (1.0 - a)));
        return out;
    }

    bool Near(const Bgra& actual, const Bgra& expected, bool withAlpha)
    {
        const auto close = [](uint8_t p, uint8_t q) { return std::abs(static_cast<int>(p) - static_cast<int>(q)) <= kTolerance; };
        return close(actual.b, expected.b) && close(actual.g, expected.g) && close(actual.r, expected.r) &&
            (!withAlpha || close(actual.a, expected.a));
    }

    // Compares every interior pixel; prints the first few mismatches.
    template <typename ExpectedAt>
    bool FrameMatches(const Frame& frame, ExpectedAt expectedAt, bool withAlpha, const char* label)
    {
        int mismatches = 0;
        for (UINT y = kMargin; y < kSize - kMargin; ++y)
        {
            for (UINT x = kMargin; x < kSize - kMargin; ++x)
            {
                const Bgra actual = frame.At(x, y);
                const Bgra expected = expectedAt(x, y);
                if (!Near(actual, expected, withAlpha))
                {
                    if (mismatches < 4)
                    {
                        std::printf("    %s (%u, %u): BGRA %3u %3u %3u %3u, expected %3u %3u %3u %3u\n", label, x, y,
                            actual.b, actual.g, actual.r, actual.a, expected.b, expected.g, expected.r, expected.a);
                    }
                    ++mismatches;
                }
            }
        }
        if (mismatches > 0)
        {
            std::printf("    %s: %d mismatching pixel(s)\n", label, mismatches);
        }
        return mismatches == 0;
    }

    bool TwinsMatch(const Frame& unorm, const Frame& srgb, const char* label)
    {
        return FrameMatches(srgb, [&](UINT x, UINT y) { return unorm.At(x, y); }, true, label);
    }

    // --- Test bodies -------------------------------------------------------------

    struct Twins
    {
        std::wstring unormPath;
        std::wstring srgbPath;
        std::vector<uint8_t> unormDds;
        std::vector<uint8_t> srgbDds;
    };

    Twins WriteTwins(Scene& scene, const Fixture& fixture)
    {
        Twins twins;
        twins.unormDds = DdsBytes(fixture, fixture.unorm);
        twins.srgbDds = DdsBytes(fixture, fixture.srgb);
        // The same texel bytes behind both headers.
        FIC2_REQUIRE(twins.unormDds.size() == twins.srgbDds.size());
        FIC2_REQUIRE(std::equal(twins.unormDds.end() - static_cast<std::ptrdiff_t>(fixture.bytes.size()), twins.unormDds.end(),
            twins.srgbDds.end() - static_cast<std::ptrdiff_t>(fixture.bytes.size())));
        twins.unormPath = scene.WriteFile(L"unorm.dds", twins.unormDds);
        twins.srgbPath = scene.WriteFile(L"srgb.dds", twins.srgbDds);
        return twins;
    }

    // Opaque oracle: both twins show the stored (block-decoded) values.
    void CheckOpaque(const Fixture& fixture, RenderPath renderPath)
    {
        std::printf("  %s, %s\n", fixture.name, PathName(renderPath));
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        scene.backplate.SetClearColor(D2D1::ColorF(kClear.r / 255.0f, kClear.g / 255.0f, kClear.b / 255.0f, 1.0f));
        const Twins twins = WriteTwins(scene, fixture);

        const auto stored = [&](UINT x, UINT y) { return fixture.decoded[static_cast<size_t>(y) * kSize + x]; };

        scene.Show(twins.unormPath, twins.unormDds, fixture.unorm, renderPath);
        const Frame unorm = scene.RenderFrame();
        scene.Show(twins.srgbPath, twins.srgbDds, fixture.srgb, renderPath);
        const Frame srgb = scene.RenderFrame();

        FIC2_CHECK(FrameMatches(unorm, stored, true, "UNORM vs stored"));
        FIC2_CHECK(FrameMatches(srgb, stored, true, "SRGB vs stored"));
        FIC2_CHECK(TwinsMatch(unorm, srgb, "SRGB vs UNORM"));
    }

    // Translucent oracle: both twins compose to the same, independently
    // computed result over each background, for the image's own (Auto) alpha
    // usage and for each explicit override.
    void CheckTranslucent(const Fixture& fixture, RenderPath renderPath)
    {
        std::printf("  %s, %s\n", fixture.name, PathName(renderPath));
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const Twins twins = WriteTwins(scene, fixture);
        const bool premultiplied = fixture.alphaMode == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED;

        const ImageCore::AlphaUsage overrides[] = {
            ImageCore::AlphaUsage::Auto, ImageCore::AlphaUsage::Coverage, ImageCore::AlphaUsage::Data };
        const Background backgrounds[] = { Background::OpaqueColor, Background::Transparent, Background::Checkerboard };

        // frames[twin][override][background]
        std::vector<Frame> frames[2];
        ImageCore::AlphaUsage effective[2][3] {};
        ImageCore::AlphaEncoding encoding[2] {};
        for (int twin = 0; twin < 2; ++twin)
        {
            const bool srgb = twin == 1;
            scene.Show(srgb ? twins.srgbPath : twins.unormPath, srgb ? twins.srgbDds : twins.unormDds,
                srgb ? fixture.srgb : fixture.unorm, renderPath);
            encoding[twin] = scene.main->GetLoadedInfo().alphaEncoding;
            for (int o = 0; o < 3; ++o)
            {
                scene.main->SetAlphaUsageOverride(overrides[o]);
                effective[twin][o] = scene.main->EffectiveAlphaUsage();
                for (const Background background : backgrounds)
                {
                    const Bgra clear = background == Background::OpaqueColor ? kClear : Bgra { 0, 0, 0, 0 };
                    scene.backplate.SetClearColor(D2D1::ColorF(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f));
                    scene.main->SetAlphaCheckerboardEnabled(background == Background::Checkerboard);
                    scene.RenderFrame();
                    frames[twin].push_back(scene.RenderFrame());
                    // The CPU fallback keeps presenting a D2D bitmap (rebuilt
                    // for a new alpha usage), never an SRV.
                    FIC2_CHECK(scene.main->GetLoadedInfo().gpuPresentation == (renderPath != RenderPath::CpuFallback));
                }
            }
            scene.main->SetAlphaUsageOverride(ImageCore::AlphaUsage::Auto);
        }

        // The twins declare the same alpha encoding and resolve the same usage.
        FIC2_CHECK(encoding[0] == encoding[1]);
        FIC2_CHECK(encoding[0] == (premultiplied ? ImageCore::AlphaEncoding::Premultiplied : ImageCore::AlphaEncoding::Unknown));
        for (int o = 0; o < 3; ++o)
        {
            FIC2_CHECK(effective[0][o] == effective[1][o]);
        }
        // Auto: transparency for an uncompressed source, data for a BCn one.
        const bool blockCompressed = DirectX::IsCompressed(fixture.unorm);
        FIC2_CHECK(effective[0][0] == (blockCompressed ? ImageCore::AlphaUsage::Data : ImageCore::AlphaUsage::Coverage));
        FIC2_CHECK(effective[0][1] == ImageCore::AlphaUsage::Coverage);
        FIC2_CHECK(effective[0][2] == ImageCore::AlphaUsage::Data);

        size_t index = 0;
        for (int o = 0; o < 3; ++o)
        {
            for (const Background background : backgrounds)
            {
                const ImageCore::AlphaUsage usage = effective[0][o];
                char label[160] {};
                std::snprintf(label, sizeof(label), "%s usage, %s",
                    usage == ImageCore::AlphaUsage::Data ? "data" : "coverage", BackgroundName(background));
                const auto expected = [&](UINT x, UINT y)
                {
                    return Compose(fixture.decoded[static_cast<size_t>(y) * kSize + x], premultiplied, usage,
                        BackgroundAt(background, x, y));
                };
                const std::string unormLabel = std::string("UNORM vs composed, ") + label;
                const std::string srgbLabel = std::string("SRGB vs composed, ") + label;
                const std::string twinLabel = std::string("SRGB vs UNORM, ") + label;
                FIC2_CHECK(FrameMatches(frames[0][index], expected, true, unormLabel.c_str()));
                FIC2_CHECK(FrameMatches(frames[1][index], expected, true, srgbLabel.c_str()));
                FIC2_CHECK(TwinsMatch(frames[0][index], frames[1][index], twinLabel.c_str()));
                ++index;
            }
        }
    }

    const RenderPath kUncompressedPaths[] = { RenderPath::GpuUncompressed, RenderPath::CpuFallback };
    const RenderPath kBlockPaths[] = { RenderPath::GpuPassthrough, RenderPath::CpuFallback };
}

FIC2_TEST(SrgbReadback_OpaqueUncompressed)
{
    for (const RenderPath path : kUncompressedPaths)
    {
        CheckOpaque(Uncompressed("R8G8B8A8 opaque", false, false, false), path);
        CheckOpaque(Uncompressed("B8G8R8A8 opaque", true, false, false), path);
    }
}

FIC2_TEST(SrgbReadback_OpaqueBlockCompressed)
{
    for (const RenderPath path : kBlockPaths)
    {
        CheckOpaque(Bc1Opaque(), path);
        CheckOpaque(Bc7(false), path);
    }
}

FIC2_TEST(SrgbReadback_TranslucentUncompressed)
{
    for (const RenderPath path : kUncompressedPaths)
    {
        CheckTranslucent(Uncompressed("R8G8B8A8 translucent (straight)", false, true, false), path);
        CheckTranslucent(Uncompressed("B8G8R8A8 translucent (straight)", true, true, false), path);
        CheckTranslucent(Uncompressed("R8G8B8A8 translucent (premultiplied)", false, true, true), path);
    }
}

FIC2_TEST(SrgbReadback_TranslucentBlockCompressed)
{
    for (const RenderPath path : kBlockPaths)
    {
        CheckTranslucent(Bc7(true), path);
    }
}
