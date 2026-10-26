#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "FD2D/Core.h"
#include "FD2D/ScrollView.h"
#include "ImageBrowser.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbnailPane.h"
#include "ImageBrowserThumbTypes.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "ThumbTestSupport.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// What a folder listing draws, read back from a software render target: the
// strip of a real image browser (on a windowless backplate, rendered into a
// WIC bitmap) after a listing, a relisting on reused tiles, a row-height
// change and a nav-items toggle. Pixels are classified by color in regions
// derived from each tile's geometry (no golden images: text antialiasing
// varies).

namespace
{
    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 900;

    // FD2D::Text draws through FD2D's DirectWrite factory: initialized for
    // these tests (and shut down after them) unless the process has it.
    struct CoreScope
    {
        bool owned { false };

        CoreScope()
        {
            FD2D::InitContext context {};
            context.instance = GetModuleHandleW(nullptr);
            owned = FD2D::Core::Initialize(context) == S_OK;
        }

        ~CoreScope()
        {
            if (owned)
            {
                FD2D::Core::Shutdown();
            }
        }
    };

    struct Rgb
    {
        int r { 0 };
        int g { 0 };
        int b { 0 };
    };

    // The canvas pixels (premultiplied BGRA over an opaque clear: plain RGB).
    struct Pixels
    {
        UINT width { 0 };
        UINT height { 0 };
        std::vector<Rgb> rgb;

        bool Read(IWICBitmap* bitmap)
        {
            if (FAILED(bitmap->GetSize(&width, &height)))
            {
                return false;
            }
            const WICRect all { 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
            Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
            if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, &lock)))
            {
                return false;
            }
            UINT stride = 0;
            UINT size = 0;
            BYTE* data = nullptr;
            if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || data == nullptr)
            {
                return false;
            }
            rgb.resize(static_cast<size_t>(width) * height);
            for (UINT y = 0; y < height; ++y)
            {
                const BYTE* row = data + static_cast<size_t>(y) * stride;
                for (UINT x = 0; x < width; ++x)
                {
                    rgb[static_cast<size_t>(y) * width + x] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
                }
            }
            return true;
        }

        // Pixels of `rect` (clamped to the canvas) that satisfy `pred`.
        template <typename Pred>
        size_t Count(D2D1_RECT_F rect, Pred pred) const
        {
            const int left = (std::max)(0, static_cast<int>(rect.left));
            const int top = (std::max)(0, static_cast<int>(rect.top));
            const int right = (std::min)(static_cast<int>(width), static_cast<int>(rect.right));
            const int bottom = (std::min)(static_cast<int>(height), static_cast<int>(rect.bottom));
            size_t count = 0;
            for (int y = top; y < bottom; ++y)
            {
                for (int x = left; x < right; ++x)
                {
                    count += pred(rgb[static_cast<size_t>(y) * width + x]) ? 1 : 0;
                }
            }
            return count;
        }

    };

    // Text and icon strokes: near white.
    bool Bright(const Rgb& c) { return c.r > 170 && c.g > 170 && c.b > 170; }
    // The selection outline accent (1.0, 0.60, 0.24) at any breathe alpha.
    bool Orange(const Rgb& c)
    {
        return c.r > 90 && c.g * 100 > c.r * 40 && c.g * 100 < c.r * 80 && c.b * 100 < c.r * 45;
    }
    // The archive-tinted icon (red tint over the white icon).
    bool Reddish(const Rgb& c) { return c.r > 150 && c.r - c.g > 50 && c.r - c.b > 50; }
    // Anything drawn over the browser background.
    bool Drawn(const Rgb& c) { return c.r + c.g + c.b > 300; }
    // The badge backdrop (near black), darker than the browser background.
    bool Dark(const Rgb& c) { return c.r + c.g + c.b < 45; }

    // Regions of a navigation tile, as ThumbNavTile lays them out.
    struct NavRegions
    {
        D2D1_RECT_F tile {};
        D2D1_RECT_F icon {};
        D2D1_RECT_F labelText {};   // the label band below the icon
        D2D1_RECT_F badge {};

        explicit NavRegions(D2D1_RECT_F r)
            : tile(r)
        {
            const float w = r.right - r.left;
            const float h = r.bottom - r.top;
            const float iconSide = (std::min)(w, h) * 0.72f;
            const float iconX = r.left + (w - iconSide) * 0.5f;
            const float iconY = r.top + h * 0.40f - iconSide * 0.5f;
            icon = D2D1::RectF(iconX, iconY, iconX + iconSide, iconY + iconSide);
            const float labelH = (std::max)(28.0f, h * 0.34f);
            labelText = D2D1::RectF(r.left + 2.0f, (std::max)(r.bottom - labelH, icon.bottom) + 1.0f,
                r.right - 2.0f, r.bottom - 2.0f);
            const float badgeH = (std::max)(18.0f, h * 0.18f);
            const float badgeW = (std::max)(36.0f, w * 0.32f);
            const float inset = (std::max)(4.0f, h * 0.05f);
            badge = D2D1::RectF(r.right - badgeW - inset, r.top + inset, r.right - inset, r.top + inset + badgeH);
        }
    };

    struct ImageFolder
    {
        std::filesystem::path dir;

        ImageFolder(const std::filesystem::path& path, size_t folders, size_t archives, size_t images)
            : dir(path)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            for (size_t i = 0; i < folders; ++i)
            {
                std::filesystem::create_directories(dir / (L"sub" + std::to_wstring(i)), ec);
            }
            const auto touch = [](const std::filesystem::path& file)
            {
                const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(handle);
                }
            };
            for (size_t i = 0; i < archives; ++i)
            {
                touch(dir / (L"pack" + std::to_wstring(i) + L".zip"));
            }
            for (size_t i = 0; i < images; ++i)
            {
                touch(dir / (L"img" + std::to_wstring(i) + L".png"));
            }
        }
    };

    struct TempRoot
    {
        std::filesystem::path dir;

        TempRoot()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2Tests_ListingPixels_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        ~TempRoot()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    // A real image browser on a windowless backplate; each frame is its
    // layout pass and a render into the canvas.
    struct PixelStrip
    {
        std::shared_ptr<FD2D::Wnd> browser;
        ThumbTestSupport::Canvas* canvas { nullptr };
        float height { 720.0f };
        Pixels pixels;

        ImageBrowserThumbnailPane& Pane() const { return *ImageBrowserThumbPane(*browser); }
        const ThumbTileSet& Tiles() const { return *Pane().StripPanel()->Tiles(); }
        IImageBrowserOps& Ops() const { return *dynamic_cast<IImageBrowserOps*>(browser.get()); }

        void Frame()
        {
            (void)browser->Measure({ static_cast<float>(kWidth), height });
            browser->Arrange({ 0.0f, 0.0f, static_cast<float>(kWidth), height });
            FIC2_CHECK(SUCCEEDED(canvas->Render(*browser)));
        }

        // Frames until the strip lists `items` items, then a few more (the
        // final update, the settled scroll and selection animation).
        bool LoadUntil(size_t items)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < deadline)
            {
                Frame();
                if (Pane().StripPanel()->Layout()->Count() == items)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        Frame();
                    }
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }

        // A frame after the selection breathe animation settles (bounded).
        void SettledFrame()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            Frame();
            FIC2_REQUIRE(pixels.Read(canvas->bitmap.Get()));
        }

        // A tile's rect on the canvas (the scroll view offsets the strip).
        D2D1_RECT_F OnCanvas(const FD2D::Wnd& tile) const
        {
            D2D1_RECT_F r = tile.LayoutRect();
            const float dx = Pane().Scroll()->ScrollX();
            r.left -= dx;
            r.right -= dx;
            return r;
        }
    };

    struct ListingExpectation
    {
        size_t navTiles { 0 };      // Up + folders + archives
        size_t archives { 0 };
        size_t images { 0 };
    };

    // Checks every tile on the canvas: navigation tiles show an icon and a
    // label (the Up tile its ".."), an archive shows the red tint and its
    // badge's dark backdrop where plain folders show neither, image tiles
    // show their caption, and the selection outline is drawn around the
    // selected tile only.
    void CheckListing(PixelStrip& strip, const ListingExpectation& expected, const char* step)
    {
        strip.SettledFrame();
        const Pixels& px = strip.pixels;

        size_t nav = 0;
        size_t archives = 0;
        size_t images = 0;
        size_t selected = 0;
        size_t outlined = 0;
        for (const ThumbTile& tile : strip.Tiles().Tiles())
        {
            const D2D1_RECT_F rect = strip.OnCanvas(*tile.focus);
            if (rect.left < 0.0f || rect.right > static_cast<float>(kWidth))
            {
                continue;   // not fully on the canvas (the prefetch margin)
            }
            const bool isSelected = tile.navTile ? tile.navTile->Selected() : tile.image->Selected();
            selected += isSelected ? 1 : 0;
            // The outline straddles the tile's top and bottom edges
            // (inflated while selected); the sides are left out, where a
            // selected neighbor's outline reaches across the gap.
            const auto edge = [&](float y)
            {
                return px.Count(D2D1::RectF(rect.left + 8.0f, y - 4.0f, rect.right - 8.0f, y + 4.0f), &Orange);
            };
            const size_t outline = edge(rect.top) + edge(rect.bottom);
            const bool hasOutline = outline > 60;
            outlined += hasOutline ? 1 : 0;
            FIC2_CHECK(hasOutline == isSelected);
            FIC2_CHECK(isSelected || outline == 0);

            if (tile.navTile)
            {
                ++nav;
                const NavRegions regions(rect);
                const size_t icon = px.Count(regions.icon, &Drawn);
                const size_t label = px.Count(regions.labelText, &Bright);
                const size_t tint = px.Count(regions.icon, &Reddish);
                const size_t backdrop = px.Count(regions.badge, &Dark);
                const bool archive = tint > 100;
                archives += archive ? 1 : 0;
                const bool up = tile.kind == ThumbItemKind::Up;
                std::printf("  [%s] %s %zu: icon %zu label %zu tint %zu badge backdrop %zu outline %zu%s\n",
                    step, up ? "up" : archive ? "archive" : "folder", tile.index, icon, label, tint, backdrop,
                    outline, isSelected ? " (selected)" : "");
                FIC2_CHECK(icon > 400);                 // the folder / folder-up icon
                FIC2_CHECK(label >= (up ? 2u : 15u));   // ".." or the folder name
                if (archive)
                {
                    FIC2_CHECK(!up);
                    FIC2_CHECK(backdrop > 150);         // the badge
                }
                else
                {
                    FIC2_CHECK(tint == 0);
                    FIC2_CHECK(backdrop < 20);
                }
            }
            else
            {
                ++images;
                const float h = rect.bottom - rect.top;
                const D2D1_RECT_F caption = D2D1::RectF(rect.left + 2.0f, rect.bottom - h * 0.25f,
                    rect.right - 2.0f, rect.bottom - 2.0f);
                const size_t text = px.Count(caption, &Bright);
                std::printf("  [%s] image %zu: caption %zu outline %zu%s\n", step, tile.index, text, outline,
                    isSelected ? " (selected)" : "");
                FIC2_CHECK(text >= 15);                 // the file name
            }
        }
        FIC2_CHECK(nav == expected.navTiles);
        FIC2_CHECK(archives == expected.archives);
        FIC2_CHECK(images == expected.images);
        FIC2_CHECK(selected == 1);
        FIC2_CHECK(outlined == 1);
    }
}

FIC2_TEST(ListingPixels_TilesDrawTheirContentAfterEachBulkUpdate)
{
    ThumbTestSupport::ComScope com;
    const CoreScope core;
    ThumbTestSupport::ScopedDecodeService decodes;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(kWidth, kHeight));
    const TempRoot root;
    const ImageFolder parent(root.dir, 3, 1, 4);

    FD2D::Backplate backplate(L"listing-pixels-test");
    PixelStrip strip;
    strip.canvas = &canvas;
    strip.browser = CreateImageBrowser(L"pixels_browser");
    FIC2_REQUIRE(strip.browser != nullptr);
    FIC2_REQUIRE(backplate.AddWnd(strip.browser));

    // Folder Up from sub0 to its parent: Up, sub0..sub2, pack0.zip and four
    // images; sub0 (where the browser came from) is selected.
    constexpr size_t kItems = 1 + 3 + 1 + 4;
    const ListingExpectation full { 1 + 3 + 1, 1, 4 };
    strip.Ops().BrowserRestoreOpenFolder((root.dir / L"sub0").wstring());
    FIC2_REQUIRE(strip.LoadUntil(1));
    strip.Ops().BrowserRestoreOpenFolder(root.dir.wstring());
    FIC2_REQUIRE(strip.LoadUntil(kItems));
    CheckListing(strip, full, "listing");

    // The same folder listed again: the tiles are reused.
    const FD2D::Wnd* firstTile = strip.Tiles().Tiles().front().focus.get();
    strip.Ops().BrowserRestoreOpenFolder(root.dir.wstring());
    FIC2_REQUIRE(strip.LoadUntil(kItems));
    FIC2_CHECK(strip.Tiles().Tiles().front().focus.get() == firstTile);
    CheckListing(strip, full, "relisting");

    // A taller pane: the row height (and every tile) grows.
    const float rowBefore = strip.Tiles().Tiles().front().focus->LayoutRect().bottom -
        strip.Tiles().Tiles().front().focus->LayoutRect().top;
    strip.height = 820.0f;
    for (int i = 0; i < 4; ++i)
    {
        strip.Frame();
    }
    const float rowAfter = strip.Tiles().Tiles().front().focus->LayoutRect().bottom -
        strip.Tiles().Tiles().front().focus->LayoutRect().top;
    std::printf("  row height %.0f -> %.0f\n", rowBefore, rowAfter);
    FIC2_CHECK(rowAfter > rowBefore + 4.0f);
    CheckListing(strip, full, "row height");

    // Nav items off: only the images; then on again.
    strip.Ops().BrowserApplyShowNavItemsForSync(false);
    FIC2_REQUIRE(strip.LoadUntil(4));
    CheckListing(strip, { 0, 0, 4 }, "nav items off");
    strip.Ops().BrowserApplyShowNavItemsForSync(true);
    FIC2_REQUIRE(strip.LoadUntil(kItems));
    CheckListing(strip, full, "nav items on");
}
