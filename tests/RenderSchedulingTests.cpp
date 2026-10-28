#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "FD2D/tests/WindowlessTestSupport.h"
#include "Ficture2Backplate.h"
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
//    synchronously, and one frame shows both panes' new view;
//  - a file drop (Ficture2Backplate::HandleFileDropPaths, also reached
//    directly by WM_DROPFILES) hit-tests a layout that changed after the last
//    frame.

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

    // Accepts a drop only inside its arranged LayoutRect, and records it.
    // Arranged into its final rect minus `leftInset`, so a test can change
    // its layout.
    class DropProbe : public FD2D::Wnd
    {
    public:
        DropProbe()
            : FD2D::Wnd(L"drop_probe")
        {
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            finalRect.x += leftInset;
            finalRect.w -= leftInset;
            FD2D::Wnd::Arrange(finalRect);
        }

        bool OnFileDrop(const std::wstring& path, const POINT& clientPt) override
        {
            UNREFERENCED_PARAMETER(path);
            const D2D1_RECT_F r = LayoutRect();
            rectAtDrop = r;
            accepted = clientPt.x >= r.left && clientPt.x < r.right && clientPt.y >= r.top && clientPt.y < r.bottom;
            return accepted;
        }

        float leftInset { 0.0f };
        D2D1_RECT_F rectAtDrop {};
        bool accepted { false };
    };

    class DropTestBackplate : public Ficture2Backplate
    {
    public:
        DropTestBackplate()
            : Ficture2Backplate(L"drop-layout-test")
        {
        }

        using Ficture2Backplate::HandleFileDropPaths;
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

FIC2_TEST(RenderScheduling_FileDropArrangesPendingLayout)
{
    constexpr UINT kDropW = 200;
    constexpr UINT kDropH = 100;
    const POINT pt { 150, 60 };
    wchar_t tempDir[MAX_PATH] {};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring folder = std::filesystem::path(tempDir).wstring();   // a browsable drop target

    struct Case
    {
        const char* name;
        std::vector<std::wstring> paths;
    };
    const Case cases[] = {
        { "single-file", { folder } },
        { "multi-file", { folder, folder } },
    };
    for (const Case& c : cases)
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        DropTestBackplate backplate;
        backplate.SkipImageBrowserIniForTesting();
        FIC2_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(kDropW, kDropH, 1.0f)));
        auto probe = std::make_shared<DropProbe>();
        probe->leftInset = 160.0f;
        FIC2_REQUIRE(backplate.AddWnd(probe));
        backplate.Render();

        // The pane layout changes after the last frame (as when an IPC open
        // adds a pane): it is only requested when the drop arrives, with no
        // render or DragOver in between.
        probe->leftInset = 0.0f;
        backplate.RequestLayout();
        const D2D1_RECT_F before = probe->LayoutRect();
        const bool staleContains = pt.x >= before.left && pt.x < before.right && pt.y >= before.top && pt.y < before.bottom;

        const bool handled = backplate.HandleFileDropPaths(c.paths, pt);
        const D2D1_RECT_F r = probe->rectAtDrop;
        std::printf("  %s drop: rect before %.0f,%.0f-%.0f,%.0f, at drop %.0f,%.0f-%.0f,%.0f, accepted %d, handled %d\n",
            c.name, before.left, before.top, before.right, before.bottom, r.left, r.top, r.right, r.bottom,
            probe->accepted ? 1 : 0, handled ? 1 : 0);
        FIC2_CHECK(!staleContains);   // the unarranged rect alone would reject the drop
        FIC2_CHECK(probe->accepted);
        FIC2_CHECK(r.left == 0.0f && r.top == 0.0f && r.right == static_cast<float>(kDropW) &&
            r.bottom == static_cast<float>(kDropH));
        FIC2_CHECK(handled);
    }
}
