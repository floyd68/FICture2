#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "FD2D/tests/WindowlessTestSupport.h"
#include "ImageBrowserMainImage.h"
#include "ScreenshotUtil.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// FICture2 paths under FD2D's coalesced rendering (#51, FD2D#4), on a
// windowless D3D backplate:
//  - a screenshot (ScreenshotUtil::SaveRenderSurfaceRectPng) renders the
//    current state immediately, also while a change is only requested;
//  - two main images synchronized through the view-changed callback (the
//    two-pane view sync) request one render together: nothing renders
//    synchronously, and one frame shows both panes' new view.

using FD2DWindowlessTest::FillRectD2DWnd;

namespace
{
    constexpr UINT kW = 64;
    constexpr UINT kH = 32;

    // Counts the frames the backplate renders.
    class FrameProbe : public FD2D::Wnd
    {
    public:
        FrameProbe()
            : FD2D::Wnd(L"frame_probe")
        {
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            UNREFERENCED_PARAMETER(target);
            ++frames;
        }

        int frames { 0 };
    };

    // Reads pixel (x, y) of a PNG as BGRA.
    bool ReadPngPixel(const std::wstring& path, UINT x, UINT y, std::uint8_t (&bgra)[4], UINT& width, UINT& height)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) ||
            FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)))
        {
            return false;
        }
        const WICRect rect { static_cast<INT>(x), static_cast<INT>(y), 1, 1 };
        return SUCCEEDED(converter->CopyPixels(&rect, 4, 4, bgra));
    }
}

FIC2_TEST(RenderScheduling_ScreenshotCapturesTheCurrentFrame)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core;
    FD2D::Backplate backplate(L"screenshot-test");
    FIC2_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(kW, kH, 1.0f)));
    auto fill = std::make_shared<FillRectD2DWnd>(L"fill", D2D1::RectF(0.0f, 0.0f, 64.0f, 32.0f),
        D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f));
    FIC2_REQUIRE(backplate.AddWnd(fill));
    backplate.Render();

    // A change that is only requested (the loop has not rendered it yet).
    fill->SetColor(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f));
    fill->Invalidate();
    FIC2_REQUIRE(backplate.IsRenderPending());
    const int framesBefore = fill->Frames();

    wchar_t tempDir[MAX_PATH] {};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring png = (std::filesystem::path(tempDir) /
        (L"FICture2Tests_Screenshot_" + std::to_wstring(GetCurrentProcessId()) + L".png")).wstring();
    const bool saved = ScreenshotUtil::SaveRenderSurfaceRectPng(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 32.0f), png);
    std::uint8_t bgra[4] {};
    UINT width = 0;
    UINT height = 0;
    const bool read = saved && ReadPngPixel(png, 10, 10, bgra, width, height);
    std::error_code ec;
    std::filesystem::remove(png, ec);
    std::printf("  screenshot saved %d, %ux%u, pixel BGRA %u %u %u %u, frames rendered by it %d, pending after %d\n",
        saved ? 1 : 0, width, height, bgra[0], bgra[1], bgra[2], bgra[3], fill->Frames() - framesBefore,
        backplate.IsRenderPending() ? 1 : 0);
    FIC2_REQUIRE(read);
    FIC2_CHECK(width == kW && height == kH);
    FIC2_CHECK(bgra[0] <= 1 && bgra[1] >= 254 && bgra[2] <= 1);   // green: the new state
    FIC2_CHECK(fill->Frames() == framesBefore + 1);
    FIC2_CHECK(!backplate.IsRenderPending());
}

FIC2_TEST(RenderScheduling_SynchronizedPanesLandInOneFrame)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core;
    FD2D::Backplate backplate(L"two-pane-sync-test");
    FIC2_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(kW, kH, 1.0f)));
    auto left = std::make_shared<ImageBrowserMainImage>(L"left");
    auto right = std::make_shared<ImageBrowserMainImage>(L"right");
    auto probe = std::make_shared<FrameProbe>();
    FIC2_REQUIRE(backplate.AddWnd(left));
    FIC2_REQUIRE(backplate.AddWnd(right));
    FIC2_REQUIRE(backplate.AddWnd(probe));
    // The view sync of the two-pane window: a change of one pane is applied
    // to the other without notifying back.
    left->SetOnViewChanged([&](const ImageViewTransform& vt) { right->SetViewTransform(vt, false); });
    backplate.Render();
    while (backplate.RenderIfPending())
    {
    }
    const int before = probe->frames;

    ImageViewTransform vt = left->GetViewTransform();
    vt.zoomScale = 2.0f;
    vt.targetZoomScale = 2.0f;
    left->SetViewTransform(vt, true);
    const int synchronous = probe->frames - before;
    const bool pending = backplate.IsRenderPending();
    (void)backplate.RenderIfPending();   // the message loop's render
    const int frames = probe->frames - before;
    std::printf("  synchronized view change: %d synchronous frame(s), pending %d, %d frame(s) after the flush; zoom %.1f / %.1f\n",
        synchronous, pending ? 1 : 0, frames, left->GetViewTransform().zoomScale, right->GetViewTransform().zoomScale);
    FIC2_CHECK(synchronous == 0);
    FIC2_CHECK(pending);
    FIC2_CHECK(frames == 1);
    FIC2_CHECK(left->GetViewTransform().zoomScale == 2.0f);
    FIC2_CHECK(right->GetViewTransform().zoomScale == 2.0f);
}
