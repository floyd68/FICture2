#include "TestHarness.h"

#include "CommonUtil.h"
#include "FD2D/Backplate.h"
#include "FD2D/ScrollView.h"
#include "FD2D/SplitPanel.h"
#include "ImageBrowser.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbnailPane.h"
#include "ImageBrowserThumbTypes.h"
#include "ThumbTestSupport.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The thumbnail strip of a real image browser on a windowless backplate: the
// browser lists a 10,000-image folder progressively, and every frame (layout
// pass plus render) materializes the range its viewport needs. Decodes go to a
// recording service that completes nothing on its own, so every request that
// is not canceled stays outstanding.

namespace
{
    constexpr size_t kImages = 10000;
    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 720;

    std::wstring ImageName(size_t i)
    {
        wchar_t name[32] {};
        swprintf_s(name, L"img%05zu.png", i);
        return name;
    }

    // Empty image files (only listed; decoding goes to the recording service).
    struct ImageFolder
    {
        std::filesystem::path dir;

        ImageFolder(const wchar_t* name, size_t count)
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (std::wstring(L"FICture2Tests_") + name + L"_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir);
            for (size_t i = 0; i < count; ++i)
            {
                const HANDLE file = CreateFileW((dir / ImageName(i)).c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(file);
                }
            }
        }

        ~ImageFolder()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    struct BrowserStrip
    {
        std::shared_ptr<FD2D::Wnd> browser;
        ThumbTestSupport::RecordingDecodeService* decodes { nullptr };
        ThumbTestSupport::Canvas* canvas { nullptr };
        size_t frames { 0 };
        size_t maxTiles { 0 };

        ImageBrowserThumbnailPane& Pane() const { return *ImageBrowserThumbPane(*browser); }
        ThumbStripPanel& Strip() const { return *Pane().StripPanel(); }
        FD2D::ScrollView& Scroll() const { return *Pane().Scroll(); }
        const ThumbStripLayout& Layout() const { return *Strip().Layout(); }
        const ThumbTileSet& Tiles() const { return *Strip().Tiles(); }

        // One frame: the layout pass, then the render.
        void Frame()
        {
            (void)browser->Measure({ static_cast<float>(kWidth), static_cast<float>(kHeight) });
            browser->Arrange({ 0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight) });
            FIC2_CHECK(SUCCEEDED(canvas->Render(*browser)));
            ++frames;
        }

        // The items in the viewport, computed as the browser does.
        std::pair<size_t, size_t> Visible() const
        {
            const D2D1_RECT_F view = Scroll().LayoutRect();
            const float width = view.right - view.left;
            const float left = view.left + Scroll().ScrollX() - Strip().LayoutRect().left;
            return Layout().Range(left, left + width);
        }

        // Tiles and outstanding thumbnail requests stay within visible + 2 * M,
        // and every visible item has a tile.
        void CheckBounds()
        {
            const auto [first, last] = Visible();
            const size_t bound = (last - first) + 2 * ThumbStripLayout::kMaxPrefetchItems;
            const ThumbIndexRange range = Tiles().Range();
            FIC2_CHECK(range.Size() <= bound);
            FIC2_CHECK(Strip().ChildrenInOrder().size() == range.Size());
            FIC2_CHECK(decodes->OutstandingThumbnails() <= bound);
            if (first < last)
            {
                FIC2_CHECK(range.first <= first && last <= range.last);
            }
            maxTiles = (std::max)(maxTiles, range.Size());
        }

        void ScrollTo(float x)
        {
            Scroll().SetScrollX(x);
            Frame();
            CheckBounds();
        }

        float ContentEnd() const
        {
            return Layout().ContentWidth();
        }

        // Frames until the strip lists `items` items (the listing is
        // progressive), checking the bounds on every frame.
        bool LoadUntil(size_t items, size_t* partialFrames = nullptr)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline)
            {
                Frame();
                CheckBounds();
                const size_t count = Layout().Count();
                if (count == items)
                {
                    // A few more frames: the final update and the settled view.
                    for (int i = 0; i < 3; ++i)
                    {
                        Frame();
                        CheckBounds();
                    }
                    return true;
                }
                if (partialFrames != nullptr && count > 0)
                {
                    ++*partialFrames;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }
    };
}

FIC2_TEST(ImageBrowserThumbStrip_TenThousandImagesStayBoundedAndScrollBackIsFree)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::ScopedDecodeService decodes;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(kWidth, kHeight));
    const ImageFolder folder(L"Strip10k", kImages);
    const ImageFolder fewImages(L"Strip5", 5);

    FD2D::Backplate backplate(L"thumb-strip-test");
    BrowserStrip strip;
    strip.browser = CreateImageBrowser(L"strip_browser");
    strip.decodes = &decodes.service;
    strip.canvas = &canvas;
    FIC2_REQUIRE(strip.browser != nullptr);
    FIC2_REQUIRE(ImageBrowserThumbPane(*strip.browser) != nullptr);
    FIC2_REQUIRE(backplate.AddWnd(strip.browser));
    auto* ops = dynamic_cast<IImageBrowserOps*>(strip.browser.get());
    FIC2_REQUIRE(ops != nullptr);

    // Open the folder (the Up item is selected: no main image is involved)
    // and let the listing arrive progressively.
    ops->BrowserRestoreOpenFolder(folder.dir.wstring());
    size_t partialFrames = 0;
    FIC2_REQUIRE(strip.LoadUntil(kImages + 1, &partialFrames));
    FIC2_CHECK(strip.Tiles().Range().first == 0);
    FIC2_CHECK(strip.Tiles().Range().Size() < 200);

    // Scrolling through the folder, then jumping to the end and back.
    const float end = strip.ContentEnd();
    const D2D1_RECT_F view = strip.Scroll().LayoutRect();
    const float viewport = view.right - view.left;
    FIC2_REQUIRE(viewport > 100.0f);
    for (float x = 0.0f; x < end; x += viewport * 3.7f)
    {
        strip.ScrollTo(x);
    }
    strip.ScrollTo(end);
    FIC2_CHECK(strip.Tiles().Find(kImages) != nullptr);   // the last item has a tile
    strip.ScrollTo(0.0f);
    FIC2_CHECK(strip.Tiles().Find(0) != nullptr);

    // Load what is materialized at the start, scroll far away and back: the
    // loaded items are shown again without a new decode request.
    std::vector<std::wstring> loaded;
    for (const auto& request : decodes.service.Requests())
    {
        if (request.request.purpose == ImageCore::ImagePurpose::Thumbnail && !request.canceled && !request.completed)
        {
            loaded.push_back(request.request.source);
            decodes.service.Complete(request.handle, S_OK, ThumbTestSupport::MakeBgraImage(2, 2));
        }
    }
    FIC2_REQUIRE(!loaded.empty());
    strip.Frame();   // presents (and caches) them
    const size_t requestsBefore = decodes.service.RequestCount();
    std::vector<size_t> requestsPerItem;
    for (const std::wstring& source : loaded)
    {
        requestsPerItem.push_back(decodes.service.ThumbnailRequestsFor(source));
    }
    strip.ScrollTo(end * 0.5f);
    strip.ScrollTo(end);
    strip.ScrollTo(0.0f);
    strip.Frame();
    for (size_t i = 0; i < loaded.size(); ++i)
    {
        FIC2_CHECK(decodes.service.ThumbnailRequestsFor(loaded[i]) == requestsPerItem[i]);
    }
    FIC2_CHECK(decodes.service.RequestCount() > requestsBefore);   // the far items were requested
    size_t shown = 0;
    for (const ThumbTile& tile : strip.Tiles().Tiles())
    {
        if (tile.image && tile.image->GetBitmapSize().width > 0.0f)
        {
            ++shown;
        }
    }
    FIC2_CHECK(shown == loaded.size());

    // Another folder: the 10,000-image folder's tiles and requests are gone.
    ops->BrowserRestoreOpenFolder(fewImages.dir.wstring());
    FIC2_REQUIRE(strip.LoadUntil(5 + 1));
    FIC2_CHECK(strip.Tiles().Range().Size() == 6);
    FIC2_CHECK(decodes.service.OutstandingThumbnails() == 5);

    std::printf("  %zu frames (%zu while the listing was partial), at most %zu tiles, %zu thumbnail requests\n",
        strip.frames, partialFrames, strip.maxTiles, decodes.service.RequestCount());
}

// A far jump animates (EnsureCentered without Immediate, as End and
// type-to-select do): the scroll view advances its offset while rendering, by
// more than the prefetch margin per frame. Every presented frame must still
// have tiles for its whole viewport.
FIC2_TEST(ImageBrowserThumbStrip_AnimatedFarJumpKeepsEveryFrameCovered)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::ScopedDecodeService decodes;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(kWidth, kHeight));
    const ImageFolder folder(L"StripJump", 1000);

    FD2D::Backplate backplate(L"thumb-strip-jump-test");
    BrowserStrip strip;
    strip.browser = CreateImageBrowser(L"strip_jump_browser");
    strip.decodes = &decodes.service;
    strip.canvas = &canvas;
    FIC2_REQUIRE(backplate.AddWnd(strip.browser));
    auto* ops = dynamic_cast<IImageBrowserOps*>(strip.browser.get());
    FIC2_REQUIRE(ops != nullptr);
    ops->BrowserRestoreOpenFolder(folder.dir.wstring());
    FIC2_REQUIRE(strip.LoadUntil(1000 + 1));
    FIC2_REQUIRE(strip.Scroll().ScrollX() == 0.0f);

    // Animates to `index` and checks every frame until the scroll settles.
    auto animateTo = [&](size_t index)
    {
        const D2D1_RECT_F panel = strip.Strip().LayoutRect();
        strip.Scroll().EnsureCentered(D2D1::RectF(
            panel.left + strip.Layout().Start(index), panel.top,
            panel.left + strip.Layout().End(index), panel.bottom), false);
        FIC2_REQUIRE(strip.Scroll().TargetScrollX() != strip.Scroll().ScrollX());
        size_t frames = 0;
        size_t moving = 0;
        while (frames < 200)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const float before = strip.Scroll().ScrollX();
            strip.Frame();   // the scroll view advances its offset while rendering
            strip.CheckBounds();   // the presented viewport has tiles
            ++frames;
            if (strip.Scroll().ScrollX() != before)
            {
                ++moving;
            }
            else if (strip.Scroll().ScrollX() == strip.Scroll().TargetScrollX())
            {
                break;
            }
        }
        FIC2_CHECK(moving >= 1);
        FIC2_CHECK(strip.Scroll().ScrollX() == strip.Scroll().TargetScrollX());
        FIC2_CHECK(strip.Tiles().Find(index) != nullptr);
    };
    animateTo(1000);   // End
    animateTo(0);      // Home
    animateTo(500);
}

// Resizing the strip (the splitter) while scrolled deep into the folder
// changes every extent during the render; the frame presented right then must
// still draw tiles where its viewport is: the visible items have tiles whose
// rectangles match the layout and lie in the rendered viewport.
FIC2_TEST(ImageBrowserThumbStrip_ResizeWhileScrolledKeepsTheFrameCovered)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::ScopedDecodeService decodes;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(kWidth, kHeight));
    const ImageFolder folder(L"StripResize", 1000);

    FD2D::Backplate backplate(L"thumb-strip-resize-test");
    BrowserStrip strip;
    strip.browser = CreateImageBrowser(L"strip_resize_browser");
    strip.decodes = &decodes.service;
    strip.canvas = &canvas;
    FIC2_REQUIRE(backplate.AddWnd(strip.browser));
    auto* ops = dynamic_cast<IImageBrowserOps*>(strip.browser.get());
    FIC2_REQUIRE(ops != nullptr);
    ops->BrowserRestoreOpenFolder(folder.dir.wstring());
    FIC2_REQUIRE(strip.LoadUntil(1000 + 1));
    const auto it = strip.browser->Children().find(L"rootSplit");
    FIC2_REQUIRE(it != strip.browser->Children().end());
    auto split = std::dynamic_pointer_cast<FD2D::SplitPanel>(it->second);
    FIC2_REQUIRE(split != nullptr);

    strip.ScrollTo(strip.ContentEnd() * 0.5f);
    float rowHeight = strip.Strip().RowHeight();
    size_t resized = 0;
    for (const float ratio : { 0.65f, 0.75f, 0.6f, 0.8f, 0.7f, 0.85f })
    {
        split->SetSplitRatio(ratio);
        strip.Frame();
        strip.CheckBounds();
        if (strip.Strip().RowHeight() != rowHeight)
        {
            ++resized;
            rowHeight = strip.Strip().RowHeight();
        }

        // Physically: each visible item's tile is where the layout puts it,
        // and inside the viewport the frame was rendered with.
        const D2D1_RECT_F view = strip.Scroll().LayoutRect();
        const float scrollX = strip.Scroll().ScrollX();
        const float panelLeft = strip.Strip().LayoutRect().left;
        const auto [first, last] = strip.Visible();
        FIC2_CHECK(first < last);
        for (size_t index = first; index < last; ++index)
        {
            const ThumbTile* tile = strip.Tiles().Find(index);
            FIC2_REQUIRE(tile != nullptr);
            const D2D1_RECT_F rect = tile->focus->LayoutRect();
            FIC2_CHECK(std::fabs(rect.left - (panelLeft + strip.Layout().Start(index))) < 0.5f);
            FIC2_CHECK(std::fabs(rect.right - (panelLeft + strip.Layout().End(index))) < 0.5f);
            const float shownLeft = rect.left - scrollX;
            const float shownRight = rect.right - scrollX;
            FIC2_CHECK(shownRight > view.left && shownLeft < view.right);
        }
    }
    FIC2_CHECK(resized >= 4);
}
