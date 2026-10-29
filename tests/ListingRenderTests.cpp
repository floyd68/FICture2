#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "DeferredRenderScope.h"
#include "FD2D/Backplate.h"
#include "FD2D/SplitPanel.h"
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
// into a hidden, never shown window through a software D2D target, and a
// probe child counts the frames. FD2D coalesces rendering (FD2D#4):
// Wnd::Invalidate() and RequestLayout() only request a render, so nothing
// renders while an update runs, and Flush() renders the pending request
// once, as the app's message loop does after the message.

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

    // A backplate that renders into a hidden window, with a frame probe.
    // Declared before anything attached to it (destroyed after it).
    struct RenderingBackplate
    {
        ThumbTestSupport::ComScope com;
        RenderWindow window;
        FD2D::Backplate backplate { L"listing-render-test" };
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

        // The message loop's render after a message: renders the pending
        // request, if any; returns the frames it rendered.
        size_t Flush()
        {
            const size_t before = Frames();
            (void)backplate.RenderIfPending();
            return Frames() - before;
        }
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
// (no render request); a changed value requests one frame and renders none
// synchronously.
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

    // Sets a value twice: the first call changes it (one frame after the
    // flush), the second does not (no request).
    const auto check = [&](const char* what, const auto& set)
    {
        const size_t before = rendering.Frames();
        set();
        const bool changedRendered = rendering.Frames() == before && rendering.Flush() == 1;
        set();
        const bool unchangedSilent = !rendering.backplate.IsRenderPending() && rendering.Flush() == 0;
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
    FIC2_CHECK(!rendering.backplate.IsRenderPending());
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
        const size_t synchronous = rendering.Frames() - before;
        const size_t frames = rendering.Flush();
        std::printf("  %s: %zu tiles, %zu synchronous frame(s), %zu frame(s) after the flush, %.1f ms\n",
            what, strip.tiles.Range().Size(), synchronous, frames, ms);
        FIC2_CHECK(synchronous == 0);
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

// Scopes nest: nothing renders inside them, the outermost requests one
// frame, layout requests made in them are laid out by that frame, and an
// exception leaves the depth balanced.
FIC2_TEST(ListingRender_NestedScopesRequestOneFrame)
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
        FIC2_CHECK(rendering.backplate.IsDeferringRender());
        nav->SetSelected(true);
    }
    FIC2_CHECK(rendering.Frames() == before);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());
    FIC2_CHECK(rendering.backplate.IsRenderPending());
    FIC2_CHECK(rendering.Flush() == 1);

    // A scope around an update that changes nothing still requests its frame.
    {
        const DeferredRenderScope scope(&rendering.backplate);
    }
    FIC2_CHECK(rendering.Flush() == 1);

    // Layout requests in nested scopes: one layout pass, in the one frame.
    const size_t layouts = rendering.probe->Layouts();
    {
        const DeferredRenderScope outer(&rendering.backplate);
        const DeferredRenderScope inner(&rendering.backplate);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        nav->SetText(L"d");
        DeferredRenderScope::RequestLayout(&rendering.backplate);
    }
    FIC2_CHECK(rendering.Frames() == before + 2);
    FIC2_CHECK(rendering.probe->Layouts() == layouts);
    FIC2_CHECK(rendering.Flush() == 1);
    FIC2_CHECK(rendering.probe->Layouts() == layouts + 1);

    // An exception leaves the scope without a synchronous frame and restores
    // the depth; its change is rendered by the next flush.
    try
    {
        const DeferredRenderScope scope(&rendering.backplate);
        nav->SetText(L"c");
        throw 1;
    }
    catch (int)
    {
    }
    FIC2_CHECK(rendering.Frames() == before + 3);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());
    FIC2_CHECK(rendering.Flush() == 1);
}

// A scope nested in a native FD2D batch (as Backplate::HandleFileDragOver
// opens one) leaves the request to the batch's owner; the changes made in it
// still render, once, with one layout pass.
FIC2_TEST(ListingRender_ScopeInsideAnFD2DBatchLeavesTheFrameToTheBatch)
{
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());
    auto nav = std::make_shared<ThumbNavTile>(L"nav");
    FIC2_REQUIRE(rendering.backplate.AddWnd(nav));
    rendering.backplate.Render();

    const size_t frames = rendering.Frames();
    const size_t layouts = rendering.probe->Layouts();
    rendering.backplate.BeginDeferredRender();
    {
        const DeferredRenderScope scope(&rendering.backplate);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        nav->SetText(L"x");
    }
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(rendering.backplate.IsDeferringRender());
    rendering.backplate.EndDeferredRender();
    rendering.backplate.RequestRender();   // the batch owner's request
    FIC2_CHECK(rendering.Flush() == 1);
    FIC2_CHECK(rendering.probe->Layouts() == layouts + 1);
}

// A split ratio changed in a scope (a control that invalidates while it is
// arranged) renders nothing synchronously, also when the scope unwinds;
// input before the frame sees the arranged panes (LayoutIfPending), and the
// flush renders them once.
FIC2_TEST(ListingRender_SplitRatioChangeInAScopeIsArrangedBeforeInputAndRenderedOnce)
{
    RenderingBackplate rendering;
    FIC2_REQUIRE(rendering.Create());
    auto split = std::make_shared<FD2D::SplitPanel>(L"split", FD2D::SplitterOrientation::Horizontal);
    auto first = std::make_shared<FD2D::Wnd>(L"first");
    auto second = std::make_shared<FD2D::Wnd>(L"second");
    split->SetFirstChild(first);
    split->SetSecondChild(second);
    FIC2_REQUIRE(rendering.backplate.AddWnd(split));
    rendering.backplate.Render();
    const auto firstWidth = [&] { return first->LayoutRect().right - first->LayoutRect().left; };
    const float half = firstWidth();
    FIC2_REQUIRE(half > 0.0f);

    // Ended by an exception.
    size_t frames = rendering.Frames();
    try
    {
        const DeferredRenderScope scope(&rendering.backplate);
        split->SetSplitRatio(0.7f);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
        throw 1;
    }
    catch (int)
    {
    }
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(!rendering.backplate.IsDeferringRender());
    FIC2_CHECK(rendering.backplate.LayoutIfPending());   // what routing the next input does
    const float wide = firstWidth();
    std::printf("  first pane %.0f -> %.0f after the unwound scope\n", half, wide);
    FIC2_CHECK(wide > half + 50.0f);
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(rendering.Flush() == 1);
    FIC2_CHECK(std::fabs(firstWidth() - wide) < 1.0f);

    // Completed normally.
    frames = rendering.Frames();
    {
        const DeferredRenderScope scope(&rendering.backplate);
        split->SetSplitRatio(0.3f);
        DeferredRenderScope::RequestLayout(&rendering.backplate);
    }
    FIC2_CHECK(rendering.Frames() == frames);
    FIC2_CHECK(rendering.Flush() == 1);
    const float narrow = firstWidth();
    std::printf("  first pane %.0f -> %.0f after the scope's frame\n", wide, narrow);
    FIC2_CHECK(narrow < half - 50.0f);
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
        size_t maxFrames { 0 };    // frames rendered by one message (flushed after it)
        size_t synchronousFrames { 0 };   // frames rendered while a message ran
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
                stats.synchronousFrames += rendering.Frames() - before;
                const size_t frames = rendering.Frames() - before + rendering.Flush();
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
        FIC2_CHECK(stats.synchronousFrames == 0);
        FIC2_CHECK(stats.maxFrames <= 1);
    };

    open("open the 158-image folder", child.dir, 1 + 158);
    open("folder up to 28 folders + 70 images", parent.dir, 1 + 28 + 70);
    open("list the parent again", parent.dir, 1 + 28 + 70);
}

// Restoring a session of several viewers inside the restore's one-frame
// scope: every viewer gets the saved strip height. The added viewers' layout
// is coalesced with that frame, so the forced height must not be computed
// from a viewer that was never arranged.
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
        FIC2_CHECK(probe->Frames() == before);   // nothing renders synchronously
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

// Viewers added beside an existing one (a folder pane, a right-side drop
// insert, a file split) after its strip height was changed get that height
// from their first layout on, and the existing strips keep theirs. A later
// height change still reaches every viewer.
FIC2_TEST(ListingRender_AddedViewersTakeTheExistingStripHeight)
{
    constexpr float kChangedHeight = 230.0f;
    constexpr float kLaterHeight = 180.0f;
    ThumbTestSupport::ScopedDecodeService decodes;
    const TempRoot root;
    const DiskFolder a(root.dir / L"a", 2, 5);
    const DiskFolder b(root.dir / L"b", 2, 5);
    const DiskFolder c(root.dir / L"c", 2, 5);

    ThumbTestSupport::ComScope com;
    RenderWindow window;
    FIC2_REQUIRE(window.Create());
    Ficture2Backplate backplate(L"strip-height-test");
    backplate.SkipImageBrowserIniForTesting();
    FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
    auto first = CreateImageBrowser(L"strip_root");
    FIC2_REQUIRE(first != nullptr);
    FIC2_REQUIRE(backplate.AddWnd(first));
    auto* ops = dynamic_cast<IImageBrowserOps*>(first.get());
    FIC2_REQUIRE(ops != nullptr);
    ops->BrowserRestoreOpenFolder(a.dir.wstring());
    backplate.Render();

    const auto stripHeight = [](FD2D::Wnd& browser)
    {
        D2D1_RECT_F strip {};
        const ImageBrowserThumbnailPane* pane = ImageBrowserThumbPane(browser);
        return (pane != nullptr && pane->TryGetStripRect(strip)) ? strip.bottom - strip.top : -1.0f;
    };
    const auto settle = [&]
    {
        for (int i = 0; i < 3; ++i)
        {
            backplate.Render();
        }
    };
    const auto checkAll = [&](const char* step, float expected)
    {
        const std::vector<FD2D::Wnd*> browsers = backplate.Bus().ImageBrowsersSnapshot();
        for (FD2D::Wnd* browser : browsers)
        {
            const float height = stripHeight(*browser);
            std::printf("  %s: %ls strip height %.1f\n", step, browser->Name().c_str(), height);
            FIC2_CHECK(std::fabs(height - expected) < 1.5f);
        }
        return browsers.size();
    };

    // The height is changed on the only viewer (what a splitter drag stores
    // and applies).
    backplate.SetSyncedThumbStripHeight(kChangedHeight);
    ops->BrowserForceApplySyncedThumbStripHeight();
    settle();
    FIC2_REQUIRE(std::fabs(stripHeight(*first) - kChangedHeight) < 1.5f);

    ops->BrowserAddHorizontalViewerForRestoreFolder(b.dir.wstring());
    settle();
    FIC2_CHECK(checkAll("folder pane added", kChangedHeight) == 2);

    ops->BrowserOpenAdditionalFilesSideBySideAfterName({ c.dir.wstring() }, first->Name());
    settle();
    FIC2_CHECK(checkAll("pane inserted after the first", kChangedHeight) == 3);

    ops->BrowserOpenAdditionalFileInHorizontalSplit((a.dir / L"img000.png").wstring());
    settle();
    FIC2_CHECK(checkAll("file split", kChangedHeight) == 4);

    // A later change (another splitter drag) still reaches every viewer.
    backplate.SynchronizeThumbStripHeight(first.get(), kLaterHeight);
    ops->BrowserForceApplySyncedThumbStripHeight();
    settle();
    FIC2_CHECK(checkAll("height changed again", kLaterHeight) == 4);
}

// The thumbnail strip keeps every tile's selection outline, at its largest,
// inside the scroll view's content viewport and clear of the horizontal
// scrollbar: at the minimum, a middle and the maximum strip height, with and
// without overflow. The row height and the viewport do not change from frame
// to frame once the strip is laid out.
FIC2_TEST(ListingRender_SelectionOutlinesStayClearOfTheScrollbar)
{
    ThumbTestSupport::ScopedDecodeService decodes;
    const TempRoot root;
    const DiskFolder many(root.dir / L"many", 3, 80);
    const DiskFolder few(root.dir / L"few", 0, 2);
    constexpr float kOutset = FD2D::SelectionMaxOutset(FD2D::SelectionStyle {});
    constexpr float kChrome = ImageBrowserThumbnailPane::kStripChrome;

    ThumbTestSupport::ComScope com;
    RenderWindow window;
    FIC2_REQUIRE(window.Create());
    Ficture2Backplate backplate(L"strip-gutter-test");
    backplate.SkipImageBrowserIniForTesting();
    FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
    auto browser = CreateImageBrowser(L"gutter_browser");
    FIC2_REQUIRE(browser != nullptr);
    FIC2_REQUIRE(backplate.AddWnd(browser));
    auto* ops = dynamic_cast<IImageBrowserOps*>(browser.get());
    FIC2_REQUIRE(ops != nullptr);
    backplate.Render();

    ImageBrowserThumbnailPane* pane = ImageBrowserThumbPane(*browser);
    FIC2_REQUIRE(pane != nullptr && pane->Scroll() != nullptr && pane->StripPanel() != nullptr);
    FD2D::ScrollView& scroll = *pane->Scroll();
    const auto settle = [&]
    {
        for (int i = 0; i < 4; ++i)
        {
            backplate.Render();
        }
    };

    struct Case
    {
        const DiskFolder* folder;
        size_t items;
        bool overflows;
    };
    const Case cases[] = { { &many, 1 + 3 + 80, true }, { &few, 1 + 2, false } };
    for (const Case& c : cases)
    {
        ops->BrowserRestoreOpenFolder(c.folder->dir.wstring());
        for (int i = 0; i < 200 && pane->StripPanel()->Layout()->Count() != c.items; ++i)
        {
            MSG msg {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                DispatchMessageW(&msg);
            }
            FD2D::CommandEvent ready {};
            ready.id = kFolderListingReadyMessage;
            (void)browser->OnCommandEvent(ready);
            backplate.Render();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FIC2_REQUIRE(pane->StripPanel()->Layout()->Count() == c.items);

        for (const float requested : { 0.0f, 150.0f, 1000.0f })   // clamped to the min and max
        {
            backplate.SetSyncedThumbStripHeight(requested);
            ops->BrowserForceApplySyncedThumbStripHeight();
            settle();

            D2D1_RECT_F strip {};
            FIC2_REQUIRE(pane->TryGetStripRect(strip));
            const float stripH = strip.bottom - strip.top;
            const float rowH = pane->StripPanel()->RowHeight();
            const D2D1_RECT_F viewport = scroll.ContentViewportRect();
            D2D1_RECT_F track {}, thumb {};
            const bool bar = scroll.HScrollBarRects(track, thumb);
            std::printf("  %s, strip %.1f: row %.1f, viewport bottom %.1f, bar %s\n",
                c.overflows ? "overflowing" : "fitting", stripH, rowH, viewport.bottom - strip.top,
                bar ? "shown" : "none");
            FIC2_CHECK(bar == c.overflows);
            FIC2_CHECK(std::fabs(rowH - (stripH - kChrome)) < 1.0f);   // the row fits the strip

            size_t checkedTiles = 0;
            for (const ThumbTile& tile : pane->StripPanel()->Tiles()->Tiles())
            {
                if (!tile.focus)
                {
                    continue;
                }
                // The outline surrounds the image, which fits in the tile.
                const D2D1_RECT_F r = tile.focus->LayoutRect();
                FIC2_CHECK(r.top - kOutset >= viewport.top - 0.01f);
                FIC2_CHECK(r.bottom + kOutset <= viewport.bottom + 0.01f);
                if (bar)
                {
                    FIC2_CHECK(r.bottom + kOutset <= track.top);
                }
                ++checkedTiles;
            }
            FIC2_CHECK(checkedTiles > 0);

            // Laid out: further frames change nothing.
            settle();
            FIC2_CHECK(pane->StripPanel()->RowHeight() == rowH);
            const D2D1_RECT_F again = scroll.ContentViewportRect();
            FIC2_CHECK(again.top == viewport.top && again.bottom == viewport.bottom);
        }
    }
}

// A viewer added while the existing strip is clamped to its max height (the
// window was enlarged after the height was set) matches it while clamped and
// after the window shrinks back and the clamp releases.
FIC2_TEST(ListingRender_AddedViewerFollowsTheStripThroughAClampedResize)
{
    constexpr float kChangedHeight = 230.0f;
    constexpr UINT kTallHeight = 1200;
    ThumbTestSupport::ScopedDecodeService decodes;
    const TempRoot root;
    const DiskFolder a(root.dir / L"a", 2, 5);
    const DiskFolder b(root.dir / L"b", 2, 5);

    ThumbTestSupport::ComScope com;
    RenderWindow window;
    FIC2_REQUIRE(window.Create());
    Ficture2Backplate backplate(L"strip-clamp-test");
    backplate.SkipImageBrowserIniForTesting();
    FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
    auto first = CreateImageBrowser(L"clamp_root");
    FIC2_REQUIRE(first != nullptr);
    FIC2_REQUIRE(backplate.AddWnd(first));
    auto* ops = dynamic_cast<IImageBrowserOps*>(first.get());
    FIC2_REQUIRE(ops != nullptr);
    ops->BrowserRestoreOpenFolder(a.dir.wstring());
    backplate.Render();

    const auto stripHeight = [](FD2D::Wnd& browser)
    {
        D2D1_RECT_F strip {};
        const ImageBrowserThumbnailPane* pane = ImageBrowserThumbPane(browser);
        return (pane != nullptr && pane->TryGetStripRect(strip)) ? strip.bottom - strip.top : -1.0f;
    };
    const auto resize = [&](UINT height)
    {
        SetWindowPos(window.window, nullptr, 0, 0, static_cast<int>(kWidth), static_cast<int>(height),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        FIC2_REQUIRE(SUCCEEDED(window.target->Resize(D2D1::SizeU(kWidth, height))));
        FD2D::BackplateTestAccess::RenderInto(backplate, window.window, window.target.Get());
        for (int i = 0; i < 3; ++i)
        {
            backplate.Render();
        }
    };

    backplate.SetSyncedThumbStripHeight(kChangedHeight);
    ops->BrowserForceApplySyncedThumbStripHeight();
    resize(kHeight);
    FIC2_REQUIRE(std::fabs(stripHeight(*first) - kChangedHeight) < 1.5f);

    // Enlarged: the strip grows with the window until its max height clamps it.
    resize(kTallHeight);
    const float clamped = stripHeight(*first);
    std::printf("  enlarged: strip height %.1f\n", clamped);
    FIC2_REQUIRE(clamped > kChangedHeight + 10.0f);

    ops->BrowserAddHorizontalViewerForRestoreFolder(b.dir.wstring());
    resize(kTallHeight);
    const std::vector<FD2D::Wnd*> browsers = backplate.Bus().ImageBrowsersSnapshot();
    FIC2_REQUIRE(browsers.size() == 2);
    for (FD2D::Wnd* browser : browsers)
    {
        std::printf("  added while clamped: %ls strip height %.1f\n", browser->Name().c_str(), stripHeight(*browser));
        FIC2_CHECK(std::fabs(stripHeight(*browser) - clamped) < 1.5f);
    }

    resize(kHeight);
    for (FD2D::Wnd* browser : browsers)
    {
        std::printf("  shrunk back: %ls strip height %.1f\n", browser->Name().c_str(), stripHeight(*browser));
        FIC2_CHECK(std::fabs(stripHeight(*browser) - kChangedHeight) < 1.5f);
    }
}
