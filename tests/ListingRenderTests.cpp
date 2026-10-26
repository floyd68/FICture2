#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "DeferredRenderScope.h"
#include "FD2D/Backplate.h"
#include "Ficture2Backplate.h"
#include "FolderEnumeration.h"
#include "IniStore.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowser.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserThumbnailPane.h"
#include "SelectionStyle.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "ThumbTestSupport.h"

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Applying a folder listing renders at most one frame. The backplate renders
// into a hidden, never shown window through a software D2D target, so
// Wnd::Invalidate() renders synchronously outside a deferred-render scope as
// it does in the app, and a probe child counts the frames.

namespace
{
    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 720;

    // A hidden window and a software HWND render target for it.
    struct RenderWindow
    {
        HWND window { nullptr };
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target;

        bool Create()
        {
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc { sizeof(wc) };
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = L"FICture2TestsRenderWindow";
            (void)RegisterClassExW(&wc);   // already registered by an earlier test: fine
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight), nullptr, nullptr, instance, nullptr);
            if (window == nullptr ||
                FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
                window, D2D1::SizeU(kWidth, kHeight), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
            return SUCCEEDED(factory->CreateHwndRenderTarget(props, hwndProps, &target));
        }

        ~RenderWindow()
        {
            target.Reset();
            if (window != nullptr)
            {
                DestroyWindow(window);
            }
        }
    };

    // Counts the frames the backplate renders.
    class RenderProbe : public FD2D::Wnd
    {
    public:
        RenderProbe()
            : Wnd(L"render_probe")
        {
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            UNREFERENCED_PARAMETER(target);
            ++m_frames;
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            ++m_layouts;   // the backplate's layout pass measures every child
            return Wnd::Measure(available);
        }

        size_t Frames() const { return m_frames; }
        size_t Layouts() const { return m_layouts; }

    private:
        size_t m_frames { 0 };
        size_t m_layouts { 0 };
    };

    // A plain FD2D backplate that can lay out without rendering, as
    // Ficture2Backplate can (DeferredLayoutHost).
    class TestBackplate : public FD2D::Backplate, public DeferredLayoutHost
    {
    public:
        using FD2D::Backplate::Backplate;

        void ArrangeChildrenNow() override
        {
            const float width = static_cast<float>(m_size.width);
            const float height = static_cast<float>(m_size.height);
            for (const auto& child : m_childrenOrdered)
            {
                (void)child->Measure({ width, height });
                child->Arrange({ 0.0f, 0.0f, width, height });
            }
        }
    };

    // A backplate that renders into a hidden window, with a frame probe.
    // Declared before anything attached to it (destroyed after it).
    struct RenderingBackplate
    {
        ThumbTestSupport::ComScope com;
        RenderWindow window;
        TestBackplate backplate { L"listing-render-test" };
        std::shared_ptr<RenderProbe> probe { std::make_shared<RenderProbe>() };

        bool Create()
        {
            if (!window.Create())
            {
                return false;
            }
            FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
            return backplate.AddWnd(probe);
        }

        size_t Frames() const { return probe->Frames(); }
    };

    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\FICture2Tests\\ListingRender"));
    const Floar::VirtualPath kOtherFolder(std::filesystem::path(L"C:\\FICture2Tests\\ListingRenderOther"));

    bool IsTestImage(const Floar::VirtualPath& path)
    {
        return path.GetExtension() == L".png";
    }

    std::wstring StableName(const wchar_t* prefix, const Floar::VirtualPath& path)
    {
        return std::wstring(prefix) + L"|" + ThumbItemModel::LookupKeyFor(path);
    }

    std::wstring Numbered(const wchar_t* stem, size_t i, const wchar_t* ext)
    {
        wchar_t name[64] {};
        swprintf_s(name, L"%s%03zu%s", stem, i, ext);
        return name;
    }

    // Folders (some of them archives, which carry a badge and a tint) and images.
    std::vector<Floar::VirtualFileEntry> Listing(const Floar::VirtualPath& folder, size_t folders, size_t archives,
        size_t images)
    {
        std::vector<Floar::VirtualFileEntry> listing;
        for (size_t i = 0; i < folders; ++i)
        {
            listing.emplace_back(Floar::VirtualPath(folder.hostPath / Numbered(L"sub", i, L"")), true, 0, 0);
        }
        for (size_t i = 0; i < archives; ++i)
        {
            listing.emplace_back(Floar::VirtualPath(folder.hostPath / Numbered(L"pack", i, L".zip")), false, 0, 0);
        }
        for (size_t i = 0; i < images; ++i)
        {
            listing.emplace_back(Floar::VirtualPath(folder.hostPath / Numbered(L"img", i, L".png")), false, 0, 0);
        }
        return listing;
    }

    // The strip controller over a panel attached to the rendering backplate.
    struct AttachedStrip
    {
        static constexpr float kRow = 96.0f;

        ThumbTestSupport::ScopedDecodeService decodes;
        std::shared_ptr<ThumbPayloadCache> payloads { std::make_shared<ThumbPayloadCache>() };
        ThumbItemModel model {};
        ThumbStripLayout layout {};
        ThumbTileSet tiles {};
        std::shared_ptr<ThumbStripPanel> panel { std::make_shared<ThumbStripPanel>() };
        ImageBrowserThumbStripController controller {};
        FD2D::Backplate* backplate { nullptr };

        explicit AttachedStrip(FD2D::Backplate& owner)
            : backplate(&owner)
        {
            panel->SetLayout(&layout);
            panel->SetTiles(&tiles);
            panel->SetRowHeight(kRow);
        }

        ~AttachedStrip()
        {
            tiles.Clear();
            panel->ClearChildren();
        }

        void List(const Floar::VirtualPath& folder, const std::vector<Floar::VirtualFileEntry>& listing)
        {
            model.Reset(folder, true, &IsTestImage);
            (void)model.Merge(listing, {});
            std::vector<float> extents(model.DisplayCount(), kRow);
            layout.Reset(std::move(extents), 4.0f, 4.0f);
        }

        void Materialize(ThumbIndexRange range, size_t selected)
        {
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = panel;
            context.backplate = backplate;
            context.tiles = &tiles;
            context.thumbW = kRow;
            context.thumbH = kRow;
            context.model = &model;
            context.range = range;
            context.visible = range;
            context.selectedIndex = selected;
            context.payloads = payloads;
            context.makeStableName = &StableName;
            controller.MaterializeTiles(context);
        }
    };
}

// Tile setters called with the value a tile already has invalidate nothing
// (no frame); a changed value renders one frame.
FIC2_TEST(ListingRender_UnchangedSetterValuesDoNotInvalidate)
{
    ThumbTestSupport::ScopedDecodeService decodes;
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());

    auto nav = std::make_shared<ThumbNavTile>(L"nav");
    auto image = std::make_shared<ThumbImageTile>(L"image");
    FIC2_REQUIRE(rendering.backplate.AddWnd(nav));
    FIC2_REQUIRE(rendering.backplate.AddWnd(image));
    rendering.backplate.Render();   // settle the first layout

    // Sets a value twice: the first call changes it (one frame), the second
    // does not (no frame).
    const auto check = [&](const char* what, const auto& set)
    {
        const size_t before = rendering.Frames();
        set();
        const bool changedRendered = rendering.Frames() == before + 1;
        set();
        const bool unchangedSilent = rendering.Frames() == before + 1;
        if (!changedRendered || !unchangedSilent)
        {
            std::printf("  setter %s: changed rendered %d, unchanged silent %d\n",
                what, changedRendered ? 1 : 0, unchangedSilent ? 1 : 0);
        }
        FIC2_CHECK(changedRendered);
        FIC2_CHECK(unchangedSilent);
    };

    check("ThumbNavTile::SetText", [&] { nav->SetText(L"folder"); });
    check("ThumbNavTile::SetBadgeText", [&] { nav->SetBadgeText(L"ZIP"); });
    check("ThumbNavTile::SetIcon", [&] { nav->SetIcon(ThumbNavTile::IconKind::Folder); });
    check("ThumbNavTile::SetIconTint", [&] { nav->SetIconTint(ThumbNavTile::IconTint::ArchiveRed); });
    check("ThumbNavTile::SetTextPlacement", [&] { nav->SetTextPlacement(ThumbNavTile::TextPlacement::Center); });
    check("ThumbNavTile::SetFixedSize", [&] { nav->SetFixedSize({ 80.0f, 80.0f }); });
    check("ThumbNavTile::SetSelected", [&] { nav->SetSelected(true); });
    check("ThumbImageTile::SetCaption", [&] { image->SetCaption(L"img.png"); });
    check("ThumbImageTile::SetFixedHeight", [&] { image->SetFixedHeight(80.0f); });
    check("ThumbImageTile::SetSelected", [&] { image->SetSelected(true); });
    check("ImageBrowserThumbImage::SetLoadingSpinnerEnabled", [&] { image->ImageWnd()->SetLoadingSpinnerEnabled(false); });
    FD2D::SelectionStyle style {};
    style.radius = 3.0f;
    check("ImageBrowserThumbImage::SetSelectionStyle", [&] { image->ImageWnd()->SetSelectionStyle(style); });

    // The values a listing sets on a reused tile bound to the same item.
    const size_t before = rendering.Frames();
    nav->SetText(L"folder");
    nav->SetBadgeText(L"ZIP");
    nav->SetIcon(ThumbNavTile::IconKind::Folder);
    nav->SetIconTint(ThumbNavTile::IconTint::ArchiveRed);
    nav->SetTextPlacement(ThumbNavTile::TextPlacement::Center);
    nav->SetFixedSize({ 80.0f, 80.0f });
    nav->SetSelected(true);
    image->SetCaption(L"img.png");
    image->SetFixedHeight(80.0f);
    image->SetSelected(true);
    FIC2_CHECK(rendering.Frames() == before);
}

// Materializing tiles through the controller renders exactly one frame: new
// tiles (navigation, archive and image tiles), the same tiles again with the
// selection moved, and another folder's items on reused (recycled) tiles.
FIC2_TEST(ListingRender_MaterializingTilesRendersOneFrame)
{
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());
    AttachedStrip strip(rendering.backplate);
    FIC2_REQUIRE(rendering.backplate.AddWnd(strip.panel));
    rendering.backplate.Render();

    strip.List(kFolder, Listing(kFolder, 28, 4, 70));
    const size_t count = strip.model.DisplayCount();
    FIC2_REQUIRE(count == 1 + 28 + 4 + 70);

    const auto materialize = [&](const char* what, ThumbIndexRange range, size_t selected)
    {
        const size_t before = rendering.Frames();
        const auto start = std::chrono::steady_clock::now();
        strip.Materialize(range, selected);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const size_t frames = rendering.Frames() - before;
        std::printf("  %s: %zu tiles, %zu frame(s), %.1f ms\n", what, strip.tiles.Range().Size(), frames, ms);
        FIC2_CHECK(frames == 1);
    };

    materialize("new tiles", { 0, count }, 0);
    FIC2_CHECK(strip.tiles.Range().Size() == count);
    materialize("same tiles, selection moved", { 0, count }, 5);
    materialize("same tiles, unchanged", { 0, count }, 5);

    // Another folder: its navigation tiles are new and its image tiles reuse
    // the released ones.
    strip.List(kOtherFolder, Listing(kOtherFolder, 10, 2, 40));
    materialize("another folder on recycled tiles", { 0, strip.model.DisplayCount() }, 0);
    FIC2_CHECK(strip.tiles.Range().Size() == strip.model.DisplayCount());
}

// Scopes nest: only the outermost renders, once, including the layout
// requests held during the scope; an exception renders nothing.
FIC2_TEST(ListingRender_NestedScopesRenderOnceAtTheOutermost)
{
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());
    auto nav = std::make_shared<ThumbNavTile>(L"nav");
    FIC2_REQUIRE(rendering.backplate.AddWnd(nav));
    rendering.backplate.Render();

    const size_t before = rendering.Frames();
    {
        const DeferredRenderScope outer(&rendering.backplate);
        {
            const DeferredRenderScope inner(&rendering.backplate);
            nav->SetText(L"a");
            nav->SetBadgeText(L"b");
        }
        FIC2_CHECK(rendering.Frames() == before);
        FIC2_CHECK(rendering.backplate.IsDeferringRender());
        nav->SetSelected(true);
        FIC2_CHECK(rendering.Frames() == before);
    }
    FIC2_CHECK(rendering.Frames() == before + 1);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());

    // Backplate::RequestLayout() renders at once even in a deferred-render
    // scope; a request made through the scope is held for its one frame,
    // which runs the layout pass.
    const size_t layouts = rendering.probe->Layouts();
    {
        const DeferredRenderScope outer(&rendering.backplate);
        const DeferredRenderScope inner(&rendering.backplate);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        nav->SetText(L"d");
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        FIC2_CHECK(rendering.Frames() == before + 1);
    }
    FIC2_CHECK(rendering.Frames() == before + 2);
    FIC2_CHECK(rendering.probe->Layouts() == layouts + 1);

    // An exception leaves the scope without a render and restores the depth.
    try
    {
        const DeferredRenderScope scope(&rendering.backplate);
        nav->SetText(L"c");
        throw 1;
    }
    catch (int)
    {
    }
    FIC2_CHECK(rendering.Frames() == before + 2);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());
}

// A layout request held by a scope that cannot render - nested in a native
// FD2D deferred-render scope, or ended by an exception - renders no frame:
// the layout pass runs without rendering, and the next frame (the enclosing
// scope's, or the queued paint) shows it.
FIC2_TEST(ListingRender_HeldLayoutRendersNothingInsideAnFD2DScopeOrWhileUnwinding)
{
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());
    auto nav = std::make_shared<ThumbNavTile>(L"nav");
    FIC2_REQUIRE(rendering.backplate.AddWnd(nav));
    rendering.backplate.Render();

    // Inside a native batch (as Backplate::HandleFileDragOver opens one).
    size_t frames = rendering.Frames();
    size_t layouts = rendering.probe->Layouts();
    rendering.backplate.BeginDeferredRender();
    {
        const DeferredRenderScope scope(&rendering.backplate);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        nav->SetText(L"x");
    }
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(rendering.probe->Layouts() == layouts + 1);
    FIC2_CHECK(rendering.backplate.IsDeferringRender());
    rendering.backplate.EndDeferredRender();
    rendering.backplate.Render();   // the native batch's frame
    FIC2_CHECK(rendering.Frames() == frames + 1);

    // Ended by an exception.
    frames = rendering.Frames();
    layouts = rendering.probe->Layouts();
    try
    {
        const DeferredRenderScope scope(&rendering.backplate);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        nav->SetText(L"y");
        throw 1;
    }
    catch (int)
    {
    }
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(rendering.probe->Layouts() == layouts + 1);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());
    rendering.backplate.Render();   // the queued paint's frame
    FIC2_CHECK(rendering.Frames() == frames + 1);
}

namespace
{
    struct DiskFolder
    {
        std::filesystem::path dir;

        DiskFolder(const std::filesystem::path& path, size_t folders, size_t images)
            : dir(path)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            for (size_t i = 0; i < folders; ++i)
            {
                std::filesystem::create_directories(dir / Numbered(L"sub", i, L""), ec);
            }
            for (size_t i = 0; i < images; ++i)
            {
                const HANDLE file = CreateFileW((dir / Numbered(L"img", i, L".png")).c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(file);
                }
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
                (L"FICture2Tests_ListingRender_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        ~TempRoot()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    struct ListingStats
    {
        size_t applies { 0 };      // folder-listing messages that changed the strip
        size_t maxFrames { 0 };    // frames rendered by one message
        size_t frames { 0 };
        double maxMs { 0.0 };      // UI-thread time of one message
    };

    // Delivers the browser's folder-listing messages (as the app's window
    // does, outside a render) until the strip lists `items` items.
    bool DeliverListing(RenderingBackplate& rendering, FD2D::Wnd& browser, size_t items, ListingStats& stats)
    {
        FolderLoadLane* lane = ImageBrowserFolderLoadLane(browser);
        const ThumbStripPanel& strip = *ImageBrowserThumbPane(browser)->StripPanel();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto settledSince = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (lane->HasChunks())
            {
                // Let the (small) listing finish, so a message applies it.
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                const size_t before = rendering.Frames();
                const auto start = std::chrono::steady_clock::now();
                (void)browser.OnCommandEvent(FD2D::CommandEvent { kFolderListingReadyMessage, 0, 0 });
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const size_t frames = rendering.Frames() - before;
                stats.frames += frames;
                stats.maxFrames = (std::max)(stats.maxFrames, frames);
                stats.maxMs = (std::max)(stats.maxMs, ms);
                stats.applies += (frames > 0) ? 1 : 0;
                settledSince = std::chrono::steady_clock::now();
            }
            else if (strip.Layout()->Count() == items &&
                std::chrono::steady_clock::now() - settledSince > std::chrono::milliseconds(300))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
}

// An image browser applying folder listings from its folder-listing message
// (DrainAsyncThumbChunks -> ApplyProgressiveThumbUpdate ->
// RebuildThumbListImmediate -> MaterializeTiles): every message renders at
// most one frame - opening a large folder, going up to its parent (new
// navigation tiles, recycled image tiles) and listing the parent again (the
// same tiles reused).
FIC2_TEST(ListingRender_ApplyingAFolderListingRendersAtMostOneFrame)
{
    ThumbTestSupport::ScopedDecodeService decodes;
    const TempRoot root;
    const DiskFolder parent(root.dir, 28, 70);
    const DiskFolder child(root.dir / Numbered(L"sub", 0, L""), 0, 158);
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());

    auto browser = CreateImageBrowser(L"listing_browser");
    FIC2_REQUIRE(browser != nullptr);
    FIC2_REQUIRE(rendering.backplate.AddWnd(browser));
    auto* ops = dynamic_cast<IImageBrowserOps*>(browser.get());
    FIC2_REQUIRE(ops != nullptr);
    rendering.backplate.Render();   // the first layout pass: the strip has a viewport

    const auto open = [&](const char* what, const std::filesystem::path& folder, size_t items)
    {
        ops->BrowserRestoreOpenFolder(folder.wstring());
        ListingStats stats {};
        FIC2_CHECK(DeliverListing(rendering, *browser, items, stats));
        std::printf("  %s: %zu items, %zu listing message(s) rendered, max %zu frame(s) and %.1f ms per message\n",
            what, items, stats.applies, stats.maxFrames, stats.maxMs);
        FIC2_CHECK(stats.applies >= 1);
        FIC2_CHECK(stats.maxFrames <= 1);
    };

    open("open the 158-image folder", child.dir, 1 + 158);
    open("folder up to 28 folders + 70 images", parent.dir, 1 + 28 + 70);
    open("list the parent again", parent.dir, 1 + 28 + 70);
}

// Restoring a session of several viewers inside the restore's one-frame
// scope: every viewer gets the saved strip height. The added viewers' layout
// requests are held for that frame, so the forced height must not be
// computed from a viewer that was never arranged.
FIC2_TEST(ListingRender_RestoredViewersGetTheSavedStripHeight)
{
    constexpr float kSavedHeight = 230.0f;
    ThumbTestSupport::ScopedDecodeService decodes;
    const TempRoot root;
    const DiskFolder a(root.dir / L"a", 2, 5);
    const DiskFolder b(root.dir / L"b", 2, 5);
    const DiskFolder c(root.dir / L"c", 2, 5);
    const std::filesystem::path folders[] = { a.dir, b.dir, c.dir };

    for (int viewers = 2; viewers <= 3; ++viewers)
    {
        // A temporary session INI (never the user's).
        const std::wstring ini = (root.dir / (L"session" + std::to_wstring(viewers) + L".ini")).wstring();
        IniStore::SetInt(ini, L"Session", L"ViewerCount", viewers);
        IniStore::SetString(ini, L"Session", L"ThumbStripHeight", L"230.00");
        for (int i = 0; i < viewers; ++i)
        {
            IniStore::SetString(ini, L"Viewer" + std::to_wstring(i), L"CurrentFolder", folders[i].wstring());
        }

        ThumbTestSupport::ComScope com;
        RenderWindow window;
        FIC2_REQUIRE(window.Create());
        Ficture2Backplate backplate(L"restore-test");
        backplate.SkipImageBrowserIniForTesting();
        FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
        auto probe = std::make_shared<RenderProbe>();
        FIC2_REQUIRE(backplate.AddWnd(probe));
        auto browser = CreateImageBrowser(L"restore_root");
        FIC2_REQUIRE(browser != nullptr);
        FIC2_REQUIRE(backplate.AddWnd(browser));
        backplate.Render();

        const size_t before = probe->Frames();
        FIC2_REQUIRE(backplate.TryRestoreImageBrowserSession(ini));
        FIC2_CHECK(probe->Frames() - before <= 1);
        for (int i = 0; i < 3; ++i)
        {
            backplate.Render();
        }

        const std::vector<FD2D::Wnd*> browsers = backplate.Bus().ImageBrowsersSnapshot();
        FIC2_CHECK(browsers.size() == static_cast<size_t>(viewers));
        for (FD2D::Wnd* restored : browsers)
        {
            D2D1_RECT_F strip {};
            const ImageBrowserThumbnailPane* pane = ImageBrowserThumbPane(*restored);
            FIC2_REQUIRE(pane != nullptr);
            FIC2_CHECK(pane->TryGetStripRect(strip));
            const float height = strip.bottom - strip.top;
            std::printf("  %d viewers: %ls strip height %.1f\n", viewers, restored->Name().c_str(), height);
            FIC2_CHECK(std::fabs(height - kSavedHeight) < 1.5f);
        }
    }
}
