#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/tests/WindowlessTestSupport.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserMainImage.h"
#include "ImageGpuResourceCache.h"

#include <windows.h>
#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Main-image presentation handoff (#42), drawn frame by frame through FD2D's
// windowless render mode: the real D3D pass, then the real D2D pass, read back
// with ReadComposedPixels(). A decode completes at a controlled point:
//   - BetweenFrames:   before Render();
//   - BeforeD3D:       in the D3D pass, in a sibling drawn before the image;
//   - BetweenPasses:   in the backplate's between-passes hook;
//   - AfterBothPasses: in the D2D pass, in a sibling drawn after the image.
// Every frame of the main-image rect must be exactly the previous image or
// exactly its replacement - never the background alone, and never the previous
// image under (or around) its replacement - and the loaded metadata must
// describe the image the frame shows.

using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::Frame;

namespace
{
    constexpr UINT kSize = 64;   // logical = surface size (DPI scale 1)
    constexpr Bgra kBackground { 0, 0, 0, 255 };

    // A test image: a w x h picture whose left half is `left` and right half
    // `right` (straight alpha; only 0 or 255). `compressed`: a BC1 payload
    // (w and h multiples of 4; alpha ignored).
    struct TestImage
    {
        uint32_t width { 16 };
        uint32_t height { 16 };
        Bgra left {};
        Bgra right {};
        bool compressed { false };
    };

    constexpr Bgra kRed { 0, 0, 255, 255 };
    constexpr Bgra kGreen { 0, 255, 0, 255 };
    constexpr Bgra kBlue { 255, 0, 0, 255 };
    constexpr Bgra kYellow { 0, 255, 255, 255 };
    constexpr Bgra kMagenta { 255, 0, 255, 255 };
    constexpr Bgra kCyan { 255, 255, 0, 255 };
    constexpr Bgra kClear { 0, 0, 0, 0 };

    TestImage Solid(Bgra color, uint32_t w = 16, uint32_t h = 16)
    {
        return TestImage { w, h, color, color, false };
    }

    uint16_t ToRgb565(const Bgra& c)
    {
        return static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    }

    ImageCore::DecodedImage Decoded(const TestImage& image)
    {
        ImageCore::DecodedImage out {};
        out.width = image.width;
        out.height = image.height;
        out.sourceMipLevels = 1;
        out.sourceMipIndex = 0;
        if (image.compressed)
        {
            // Solid BC1: both endpoints the color, every index 0. Exact for the
            // fully saturated test colors.
            const uint32_t blocksX = (image.width + 3) / 4;
            const uint32_t blocksY = (image.height + 3) / 4;
            const uint16_t c = ToRgb565(image.left);
            out.dxgiFormat = DXGI_FORMAT_BC1_UNORM;
            out.rowPitchBytes = blocksX * 8u;
            out.blocks = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(blocksX) * blocksY * 8u, uint8_t(0));
            for (size_t block = 0; block < static_cast<size_t>(blocksX) * blocksY; ++block)
            {
                uint8_t* p = out.blocks->data() + block * 8u;
                p[0] = static_cast<uint8_t>(c & 0xFF);
                p[1] = static_cast<uint8_t>(c >> 8);
                p[2] = static_cast<uint8_t>(c & 0xFF);
                p[3] = static_cast<uint8_t>(c >> 8);
            }
        }
        else
        {
            out.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            out.rowPitchBytes = image.width * 4u;
            out.blocks = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(image.width) * image.height * 4u);
            for (uint32_t y = 0; y < image.height; ++y)
            {
                for (uint32_t x = 0; x < image.width; ++x)
                {
                    const Bgra& c = (x < image.width / 2) ? image.left : image.right;
                    uint8_t* p = out.blocks->data() + (static_cast<size_t>(y) * image.width + x) * 4u;
                    p[0] = c.b;
                    p[1] = c.g;
                    p[2] = c.r;
                    p[3] = c.a;
                }
            }
        }
        out.blockBytes = static_cast<uint32_t>(out.blocks->size());
        return out;
    }

    // Top-level BC1 dimensions that are not multiples of 4: the D3D texture
    // cannot be created, so the main image falls back to a CPU decode that it
    // presents as a D2D bitmap (the real forced-D2D path).
    ImageCore::DecodedImage UncreatableBc1()
    {
        TestImage image = Solid(kRed, 2, 2);
        image.compressed = true;
        return Decoded(image);
    }

    // What a frame of the main-image rect must look like, as probe points.
    struct Look
    {
        const char* name { "" };
        std::vector<std::pair<POINT, Bgra>> probes;
    };

    // The image aspect-fit into the kSize x kSize rect (zoom 1, no pan), over
    // the background. Probes are a 4 px grid, skipping points within the
    // filter footprint (2 source texels) of the image edges and of its
    // left/right seam.
    Look LookOf(const char* name, const TestImage& image)
    {
        Look look { name, {} };
        const float scale = (std::min)(static_cast<float>(kSize) / image.width, static_cast<float>(kSize) / image.height);
        const float w = image.width * scale;
        const float h = image.height * scale;
        const float left = (kSize - w) * 0.5f;
        const float top = (kSize - h) * 0.5f;
        const float seam = left + w * 0.5f;
        const float margin = 2.0f * scale + 1.5f;
        const auto withinMargin = [margin](float a, float b) { return std::abs(a - b) < margin; };
        const bool twoHalves = image.left.a != image.right.a || image.left.b != image.right.b ||
            image.left.g != image.right.g || image.left.r != image.right.r;
        for (int y = 2; y < static_cast<int>(kSize); y += 4)
        {
            for (int x = 2; x < static_cast<int>(kSize); x += 4)
            {
                const float cx = x + 0.5f;
                const float cy = y + 0.5f;
                if (withinMargin(cx, left) || withinMargin(cx, left + w) || withinMargin(cy, top) || withinMargin(cy, top + h) ||
                    (twoHalves && withinMargin(cx, seam)))
                {
                    continue;
                }
                const bool inside = cx > left && cx < left + w && cy > top && cy < top + h;
                Bgra expected = kBackground;
                if (inside)
                {
                    const Bgra& c = (cx < seam) ? image.left : image.right;
                    if (c.a != 0 || image.compressed)
                    {
                        expected = Bgra { c.b, c.g, c.r, 255 };
                    }
                }
                look.probes.push_back({ POINT { x, y }, expected });
            }
        }
        return look;
    }

    Look BackgroundLook()
    {
        return LookOf("background", Solid(kClear));
    }

    bool Matches(const Frame& frame, const Look& look)
    {
        for (const auto& [pt, expected] : look.probes)
        {
            const Bgra actual = frame.At(static_cast<UINT>(pt.x), static_cast<UINT>(pt.y));
            const auto close = [](uint8_t a, uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 2; };
            if (!close(actual.b, expected.b) || !close(actual.g, expected.g) || !close(actual.r, expected.r))
            {
                return false;
            }
        }
        return true;
    }

    // Records decode requests and completes or fails them on demand, on the
    // calling (UI) thread.
    class ControlledDecodeService : public IImageDecodeService
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

        void Complete(size_t index, ImageCore::DecodedImage image)
        {
            requests.at(index).callback(S_OK, std::move(image));
        }
        void Fail(size_t index)
        {
            requests.at(index).callback(E_FAIL, ImageCore::DecodedImage {});
        }
        size_t Latest() const { return requests.size() - 1; }

        std::vector<Request> requests;
    };

    // Draws nothing; runs a one-shot action in its D3D or D2D pass.
    class ProbeWnd : public FD2D::Wnd
    {
    public:
        using FD2D::Wnd::Wnd;
        std::function<void()> onD3D {};
        std::function<void()> onD2D {};

        void OnRenderD3D(ID3D11DeviceContext*) override { RunOnce(onD3D); }
        void OnRender(ID2D1RenderTarget*) override { RunOnce(onD2D); }

        static void RunOnce(std::function<void()>& action)
        {
            if (action)
            {
                std::function<void()> run = std::move(action);
                action = nullptr;
                run();
            }
        }
    };

    enum class Timing
    {
        BetweenFrames,
        BeforeD3D,
        BetweenPasses,
        AfterBothPasses,
    };

    constexpr Timing kTimings[] = { Timing::BetweenFrames, Timing::BeforeD3D, Timing::BetweenPasses, Timing::AfterBothPasses };

    const char* TimingName(Timing timing)
    {
        switch (timing)
        {
        case Timing::BetweenFrames: return "between frames";
        case Timing::BeforeD3D: return "before the D3D pass";
        case Timing::BetweenPasses: return "between the passes";
        default: return "after both passes";
        }
    }

    // Completion before the image's handoff point (its D3D pass) is shown by
    // that frame; completion after it waits exactly one frame.
    int FirstReplacementFrame(Timing timing)
    {
        return (timing == Timing::BetweenFrames || timing == Timing::BeforeD3D) ? 0 : 1;
    }

    // Routes decodes to `service` for its lifetime; declared before the
    // backplate so the main image's binding is gone before it is restored.
    struct ScopedDecodeService
    {
        explicit ScopedDecodeService(IImageDecodeService* service)
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(service);
            ImageGpuResourceCache::Instance().Clear();
        }
        ~ScopedDecodeService()
        {
            ImageGpuResourceCache::Instance().Clear();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }
    };

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        ControlledDecodeService decodes;
        ScopedDecodeService decodeScope { &decodes };
        std::filesystem::path dir;
        FD2D::Backplate backplate { L"main-image-handoff-test" };
        std::shared_ptr<ProbeWnd> before { std::make_shared<ProbeWnd>(L"before") };
        std::shared_ptr<ImageBrowserMainImage> main { std::make_shared<ImageBrowserMainImage>(L"main") };
        std::shared_ptr<ProbeWnd> after { std::make_shared<ProbeWnd>(L"after") };
        std::function<void()> betweenPasses {};

        ~Scene()
        {
            backplate.SetBetweenPassesHook({});
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        bool Create()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2HandoffTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            if (FAILED(backplate.InitializeWindowless(kSize, kSize)))
            {
                return false;
            }
            backplate.SetBetweenPassesHook([this]() { ProbeWnd::RunOnce(betweenPasses); });
            main->SetLoadingSpinnerEnabled(false);
            return backplate.AddWnd(before) && backplate.AddWnd(main) && backplate.AddWnd(after);
        }

        // A real file, so the source identity (and with it the GPU cache) works.
        std::wstring File(const wchar_t* name)
        {
            const std::filesystem::path file = dir / name;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "FICture2 handoff test";
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

        // Runs `action` at `timing` of the next Render() (BetweenFrames: now).
        void At(Timing timing, std::function<void()> action)
        {
            switch (timing)
            {
            case Timing::BetweenFrames: action(); break;
            case Timing::BeforeD3D: before->onD3D = std::move(action); break;
            case Timing::BetweenPasses: betweenPasses = std::move(action); break;
            case Timing::AfterBothPasses: after->onD2D = std::move(action); break;
            }
        }

        // Selects `path` and renders the frame that dispatches its decode.
        Frame Select(const std::wstring& path)
        {
            FIC2_CHECK(SUCCEEDED(main->SetSourceFile(path)));
            return RenderFrame();
        }

        // Selects and shows an image through a decode (completed between frames).
        bool Show(const std::wstring& path, const TestImage& image)
        {
            Select(path);
            if (decodes.requests.empty() || decodes.requests.back().request.source != path)
            {
                return false;
            }
            decodes.Complete(decodes.Latest(), Decoded(image));
            return RenderUntilShown(path, image);
        }

        // Selects and shows an image through the forced D2D fallback.
        bool ShowAsBitmap(const std::wstring& path, const TestImage& image)
        {
            Select(path);
            if (decodes.requests.empty())
            {
                return false;
            }
            decodes.Complete(decodes.Latest(), UncreatableBc1());
            const size_t before = decodes.requests.size();
            RenderFrame();
            if (decodes.requests.size() != before + 1 || decodes.requests.back().request.allowGpuCompressedDDS)
            {
                return false;
            }
            decodes.Complete(decodes.Latest(), Decoded(image));
            return RenderUntilShown(path, image) && !main->GetLoadedInfo().gpuPresentation;
        }

        // Setup only (the handoff under test is checked frame by frame): the
        // first image of an empty pane may take a frame longer without the fix.
        bool RenderUntilShown(const std::wstring& path, const TestImage& image)
        {
            for (int i = 0; i < 2; ++i)
            {
                if (Matches(RenderFrame(), LookOf("shown", image)) && main->GetLoadedInfo().sourcePath == path)
                {
                    return true;
                }
            }
            return false;
        }
    };

    // Which look a frame shows (index into `looks`), -1 for none; prints the
    // frame's probe row on a mismatch.
    int Classify(const Frame& frame, const std::vector<Look>& looks)
    {
        for (size_t i = 0; i < looks.size(); ++i)
        {
            if (Matches(frame, looks[i]))
            {
                return static_cast<int>(i);
            }
        }
        frame.Print("    unexpected frame", kSize / 2);
        return -1;
    }

    // One handoff from `oldImage` (shown) to `newImage`, whose decode completes
    // at `timing`. `newAsBitmap`: the replacement goes through the forced D2D
    // fallback. Checks every frame and the metadata that goes with it.
    void CheckHandoff(const char* label, Timing timing, const TestImage& oldImage, bool oldAsBitmap,
        const TestImage& newImage, bool newAsBitmap)
    {
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const std::wstring oldPath = scene.File(L"old.png");
        const std::wstring newPath = scene.File(L"new.png");
        FIC2_REQUIRE(oldAsBitmap ? scene.ShowAsBitmap(oldPath, oldImage) : scene.Show(oldPath, oldImage));

        const std::vector<Look> looks { LookOf("old", oldImage), LookOf("new", newImage), BackgroundLook() };
        std::vector<int> shown;
        const auto record = [&](const Frame& frame)
        {
            const int look = Classify(frame, looks);
            shown.push_back(look);
            // The metadata describes what the frame shows.
            const ImageLoadedInfo info = scene.main->GetLoadedInfo();
            if (look == 0)
            {
                FIC2_CHECK(info.sourcePath == oldPath);
                FIC2_CHECK(info.width == oldImage.width && info.height == oldImage.height);
            }
            else if (look == 1)
            {
                FIC2_CHECK(info.sourcePath == newPath);
                FIC2_CHECK(info.width == newImage.width && info.height == newImage.height);
                FIC2_CHECK(info.gpuPresentation == !newAsBitmap);
            }
        };

        record(scene.Select(newPath));   // dispatches the decode
        FIC2_REQUIRE(!scene.decodes.requests.empty());
        if (newAsBitmap)
        {
            // The compressed result cannot become a texture: a CPU decode follows.
            scene.decodes.Complete(scene.decodes.Latest(), UncreatableBc1());
            const size_t requests = scene.decodes.requests.size();
            record(scene.RenderFrame());
            FIC2_REQUIRE(scene.decodes.requests.size() == requests + 1);
            FIC2_CHECK(!scene.decodes.requests.back().request.allowGpuCompressedDDS);
        }

        const size_t request = scene.decodes.Latest();
        const size_t handoffStart = shown.size();
        scene.At(timing, [&]() { scene.decodes.Complete(request, Decoded(newImage)); });
        for (int i = 0; i < 3; ++i)
        {
            record(scene.RenderFrame());
        }

        std::printf("  %-34s %-20s frames:", label, TimingName(timing));
        for (const int look : shown)
        {
            std::printf(" %s", look < 0 ? "OTHER" : looks[static_cast<size_t>(look)].name);
        }
        std::printf("\n");

        // Previous image until the handoff, then the replacement; nothing else.
        const size_t firstNew = handoffStart + static_cast<size_t>(FirstReplacementFrame(timing));
        for (size_t i = 0; i < shown.size(); ++i)
        {
            FIC2_CHECK(shown[i] == (i < firstNew ? 0 : 1));
        }
        FIC2_CHECK(scene.decodes.requests.size() == request + 1);   // no extra decode
    }

    void CheckHandoffAtEveryTiming(const char* label, const TestImage& oldImage, bool oldAsBitmap,
        const TestImage& newImage, bool newAsBitmap)
    {
        for (const Timing timing : kTimings)
        {
            CheckHandoff(label, timing, oldImage, oldAsBitmap, newImage, newAsBitmap);
        }
    }

    TestImage Bc1(Bgra color, uint32_t w = 16, uint32_t h = 16)
    {
        TestImage image = Solid(color, w, h);
        image.compressed = true;
        return image;
    }
}

FIC2_TEST(MainImageHandoff_UncachedBgraReplacesWithoutBackgroundFrame)
{
    CheckHandoffAtEveryTiming("BGRA -> BGRA", Solid(kRed), false, Solid(kGreen), false);
}

FIC2_TEST(MainImageHandoff_TransparentReplacementNeverShowsOldImageUnderneath)
{
    // The replacement's left half is fully transparent: the background must
    // show there, not the previous image.
    const TestImage halfTransparent { 16, 16, kClear, kGreen, false };
    CheckHandoffAtEveryTiming("BGRA -> transparent BGRA", Solid(kRed), false, halfTransparent, false);
    CheckHandoffAtEveryTiming("transparent BGRA -> BGRA", halfTransparent, false, Solid(kBlue), false);
}

FIC2_TEST(MainImageHandoff_DifferentDimensionsNeverShowOldImageAround)
{
    // Wide to tall and back: the previous image's area outside the replacement
    // must be background once the replacement shows.
    CheckHandoffAtEveryTiming("wide -> tall", Solid(kRed, 32, 8), false, Solid(kGreen, 8, 32), false);
    CheckHandoffAtEveryTiming("tall -> wide", Solid(kYellow, 8, 32), false, Solid(kBlue, 32, 8), false);
}

FIC2_TEST(MainImageHandoff_BgraAndCompressedGpuTransitions)
{
    CheckHandoffAtEveryTiming("BGRA -> BC1", Solid(kRed), false, Bc1(kGreen), false);
    CheckHandoffAtEveryTiming("BC1 -> BGRA", Bc1(kBlue, 32, 16), false, Solid(kYellow, 16, 32), false);
    CheckHandoffAtEveryTiming("BC1 -> BC1", Bc1(kRed, 16, 32), false, Bc1(kCyan, 32, 16), false);
}

FIC2_TEST(MainImageHandoff_ForcedD2DFallbackTransitions)
{
    const TestImage halfTransparent { 16, 16, kClear, kGreen, false };
    // SRV -> D2D bitmap: the old SRV must not be drawn under the new bitmap.
    CheckHandoffAtEveryTiming("SRV -> bitmap (transparent)", Solid(kRed), false, halfTransparent, true);
    CheckHandoffAtEveryTiming("SRV -> bitmap (other size)", Solid(kRed, 32, 8), false, Solid(kBlue, 8, 32), true);
    // D2D bitmap -> SRV: the old bitmap must stay until the SRV is drawn.
    CheckHandoffAtEveryTiming("bitmap -> SRV", Solid(kMagenta), true, Solid(kGreen, 8, 32), false);
    CheckHandoffAtEveryTiming("bitmap -> BC1", Solid(kMagenta, 32, 8), true, Bc1(kCyan), false);
    CheckHandoffAtEveryTiming("bitmap -> bitmap", Solid(kYellow, 32, 8), true, halfTransparent, true);
}

FIC2_TEST(MainImageHandoff_FailedDecodeKeepsPreviousImage)
{
    for (const Timing timing : kTimings)
    {
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const std::wstring a = scene.File(L"a.png");
        const std::wstring broken = scene.File(L"broken.png");
        const std::wstring c = scene.File(L"c.png");
        FIC2_REQUIRE(scene.Show(a, Solid(kRed)));
        const Look red = LookOf("red", Solid(kRed));

        FIC2_CHECK(Matches(scene.Select(broken), red));
        const size_t request = scene.decodes.Latest();
        scene.At(timing, [&]() { scene.decodes.Fail(request); });
        int redFrames = 0;
        for (int i = 0; i < 3; ++i)
        {
            redFrames += Matches(scene.RenderFrame(), red) ? 1 : 0;
        }
        std::printf("  failed decode %-20s previous image in %d/3 frames\n", TimingName(timing), redFrames);
        FIC2_CHECK(redFrames == 3);
        FIC2_CHECK(scene.decodes.requests.size() == request + 1);   // no retry loop
        FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == a);

        // The next valid image hands off normally.
        FIC2_CHECK(Matches(scene.Select(c), red));
        scene.decodes.Complete(scene.decodes.Latest(), Decoded(Solid(kGreen)));
        FIC2_CHECK(Matches(scene.RenderFrame(), LookOf("green", Solid(kGreen))));
        FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == c);
    }
}

FIC2_TEST(MainImageHandoff_CacheHitIsImmediate)
{
    Scene scene;
    FIC2_REQUIRE(scene.Create());
    const std::wstring a = scene.File(L"a.png");
    const std::wstring b = scene.File(L"b.png");
    FIC2_REQUIRE(scene.Show(a, Solid(kRed, 32, 8)));
    FIC2_REQUIRE(scene.Show(b, Solid(kGreen)));
    const size_t requests = scene.decodes.requests.size();

    // Revisit: presented inside SetSourceFile, drawn by the very next frame,
    // no decode.
    FIC2_CHECK(scene.main->SetSourceFile(a) == S_OK);
    FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == a);
    FIC2_CHECK(scene.main->GetLoadedInfo().gpuPresentation);
    FIC2_CHECK(Matches(scene.RenderFrame(), LookOf("a", Solid(kRed, 32, 8))));
    FIC2_CHECK(scene.decodes.requests.size() == requests);

    FIC2_CHECK(scene.main->SetSourceFile(b) == S_OK);
    FIC2_CHECK(Matches(scene.RenderFrame(), LookOf("b", Solid(kGreen))));
    FIC2_CHECK(scene.decodes.requests.size() == requests);
}

FIC2_TEST(MainImageHandoff_CacheHitBetweenPassesWaitsForNextFrame)
{
    // A selection made while a frame is between its passes (for example by a
    // listing update in the D2D pass of a parent) hits the cache: that frame
    // keeps the previous image and the next one shows the hit. The previous
    // image is an SRV and, in the second case, a D2D bitmap - which the D2D
    // pass would no longer have drawn if the hit replaced it mid-frame.
    for (const bool previousAsBitmap : { false, true })
    {
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const std::wstring a = scene.File(L"a.png");
        const std::wstring b = scene.File(L"b.png");
        FIC2_REQUIRE(scene.Show(a, Solid(kRed, 32, 8)));
        FIC2_REQUIRE(previousAsBitmap ? scene.ShowAsBitmap(b, Solid(kGreen, 8, 32)) : scene.Show(b, Solid(kGreen, 8, 32)));
        const size_t requests = scene.decodes.requests.size();
        const Look lookA = LookOf("a", Solid(kRed, 32, 8));
        const Look lookB = LookOf("b", Solid(kGreen, 8, 32));

        scene.At(Timing::BetweenPasses, [&]() { FIC2_CHECK(scene.main->SetSourceFile(a) == S_OK); });
        const Frame first = scene.RenderFrame();
        const bool firstIsPrevious = Matches(first, lookB);
        const bool metadataIsPrevious = scene.main->GetLoadedInfo().sourcePath == b;
        const Frame second = scene.RenderFrame();
        const bool secondIsHit = Matches(second, lookA);
        std::printf("  cache hit between passes, previous %s: first frame previous %d, second frame hit %d\n",
            previousAsBitmap ? "bitmap" : "SRV", firstIsPrevious ? 1 : 0, secondIsHit ? 1 : 0);
        FIC2_CHECK(firstIsPrevious);
        FIC2_CHECK(metadataIsPrevious);
        FIC2_CHECK(secondIsHit);
        FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == a);
        FIC2_CHECK(scene.decodes.requests.size() == requests);   // still a cache hit, no decode
    }
}

FIC2_TEST(MainImageHandoff_SupersededCompletionNeverReplacesCurrentRequest)
{
    for (const Timing timing : kTimings)
    {
        Scene scene;
        FIC2_REQUIRE(scene.Create());
        const std::wstring a = scene.File(L"a.png");
        const std::wstring b = scene.File(L"b.png");
        const std::wstring c = scene.File(L"c.png");
        FIC2_REQUIRE(scene.Show(a, Solid(kRed)));
        const Look red = LookOf("red", Solid(kRed));

        scene.Select(b);
        const size_t requestB = scene.decodes.Latest();
        FIC2_CHECK(Matches(scene.Select(c), red));
        const size_t requestC = scene.decodes.Latest();
        FIC2_REQUIRE(requestC == requestB + 1);

        // b completes after c was requested: never shown.
        scene.At(timing, [&]() { scene.decodes.Complete(requestB, Decoded(Solid(kBlue))); });
        int redFrames = 0;
        for (int i = 0; i < 2; ++i)
        {
            redFrames += Matches(scene.RenderFrame(), red) ? 1 : 0;
        }
        std::printf("  superseded completion %-20s previous image in %d/2 frames\n", TimingName(timing), redFrames);
        FIC2_CHECK(redFrames == 2);
        FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == a);

        scene.At(timing, [&]() { scene.decodes.Complete(requestC, Decoded(Solid(kGreen))); });
        const Look green = LookOf("green", Solid(kGreen));
        std::vector<bool> isGreen;
        for (int i = 0; i < 2; ++i)
        {
            const Frame frame = scene.RenderFrame();
            isGreen.push_back(Matches(frame, green));
            FIC2_CHECK(isGreen.back() || Matches(frame, red));
        }
        FIC2_CHECK(isGreen.back());
        FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == c);
    }

    // A completion already staged when the next image is selected is dropped.
    Scene scene;
    FIC2_REQUIRE(scene.Create());
    const std::wstring a = scene.File(L"a.png");
    const std::wstring b = scene.File(L"b.png");
    const std::wstring c = scene.File(L"c.png");
    FIC2_REQUIRE(scene.Show(a, Solid(kRed)));
    scene.Select(b);
    scene.decodes.Complete(scene.decodes.Latest(), Decoded(Solid(kBlue)));
    FIC2_CHECK(Matches(scene.Select(c), LookOf("red", Solid(kRed))));
    FIC2_CHECK(Matches(scene.RenderFrame(), LookOf("red", Solid(kRed))));
    FIC2_CHECK(scene.main->GetLoadedInfo().sourcePath == a);
}

FIC2_TEST(MainImageHandoff_RapidNavigationShowsOnlyCompletedCurrentImages)
{
    // First visits, superseded loads and a cached revisit in quick succession,
    // as with holding the next-image key: every frame is the last image that
    // was presented for a request, never a superseded one or the background.
    Scene scene;
    FIC2_REQUIRE(scene.Create());
    const TestImage images[] = {
        Solid(kRed), Solid(kGreen, 32, 8), Solid(kBlue, 8, 32), TestImage { 16, 16, kClear, kYellow, false },
        Bc1(kCyan, 32, 16),
    };
    std::vector<std::wstring> paths;
    std::vector<Look> looks;
    for (size_t i = 0; i < std::size(images); ++i)
    {
        paths.push_back(scene.File((L"rapid" + std::to_wstring(i) + L".png").c_str()));
        looks.push_back(LookOf("rapid", images[i]));
    }
    looks.push_back(BackgroundLook());
    FIC2_REQUIRE(scene.Show(paths[0], images[0]));

    std::vector<int> shown;
    const auto frame = [&]() { shown.push_back(Classify(scene.RenderFrame(), looks)); };

    const auto select = [&](size_t i)
    {
        shown.push_back(Classify(scene.Select(paths[i]), looks));
        return scene.decodes.Latest();
    };

    // 1 and 2 are passed over before their decodes complete.
    const size_t request1 = select(1);
    const size_t request2 = select(2);
    const size_t request3 = select(3);
    scene.At(Timing::BeforeD3D, [&]() { scene.decodes.Complete(request1, Decoded(images[1])); });
    frame();
    scene.At(Timing::BetweenPasses, [&]() { scene.decodes.Complete(request2, Decoded(images[2])); });
    frame();
    // 3 completes between the passes: shown from the next frame.
    scene.At(Timing::BetweenPasses, [&]() { scene.decodes.Complete(request3, Decoded(images[3])); });
    frame();
    frame();
    // 4 completes after both passes; then a cached revisit of 0.
    const size_t request4 = select(4);
    scene.At(Timing::AfterBothPasses, [&]() { scene.decodes.Complete(request4, Decoded(images[4])); });
    frame();
    frame();
    const size_t requestsBeforeRevisit = scene.decodes.requests.size();
    FIC2_CHECK(scene.main->SetSourceFile(paths[0]) == S_OK);
    frame();
    FIC2_CHECK(scene.decodes.requests.size() == requestsBeforeRevisit);

    std::printf("  rapid navigation frames:");
    for (const int look : shown)
    {
        std::printf(" %d", look);
    }
    std::printf("\n");
    const std::vector<int> expected { 0, 0, 0, 0, 0, 0, 3, 3, 3, 4, 0 };
    FIC2_CHECK(shown == expected);
}
