#include "TestHarness.h"

#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbPayloadStore.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserThumbnailPane.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "ThumbTestSupport.h"
#include "CommonUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Strip tiles over model indices: only the materialized index range has tiles,
// the panel places each tile at its model index's layout position, and the
// selection is a model index that a tile shows when it is bound. Ranges here
// are restricted to a subset of the model, as visible-range materialization
// will do.

namespace
{
    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }

    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\FICture2Tests\\Strip"));

    Floar::VirtualFileEntry File(const std::wstring& name)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(kFolder.hostPath / name), false, 0, 0);
    }

    Floar::VirtualFileEntry Dir(const std::wstring& name)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(kFolder.hostPath / name), true, 0, 0);
    }

    bool IsTestImage(const Floar::VirtualPath& path)
    {
        return path.GetExtension() == L".png";
    }

    std::wstring StableName(const wchar_t* prefix, const Floar::VirtualPath& path)
    {
        return std::wstring(prefix) + L"|" + ThumbItemModel::LookupKeyFor(path);
    }

    // Up, two folders, then img00.png .. img11.png: 15 items, images from index 3.
    struct Strip
    {
        ThumbTestSupport::RecordingDecodeService decodes {};
        std::shared_ptr<ThumbPayloadStore> payloads { std::make_shared<ThumbPayloadStore>() };
        ThumbItemModel model {};
        ThumbTileSet tiles {};
        std::shared_ptr<ThumbStripPanel> panel { std::make_shared<ThumbStripPanel>() };
        ImageBrowserThumbStripController controller {};
        size_t selected { static_cast<size_t>(-1) };

        Strip()
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(&decodes);
            model.Reset(kFolder, true, &IsTestImage);
            std::vector<Floar::VirtualFileEntry> listing { Dir(L"sub1"), Dir(L"sub2") };
            for (int i = 0; i < 12; ++i)
            {
                listing.push_back(File(L"img" + std::wstring(i < 10 ? L"0" : L"") + std::to_wstring(i) + L".png"));
            }
            (void)model.Merge(listing);
            panel->SetTiles(&tiles);
        }

        ~Strip()
        {
            tiles.Clear();
            panel->ClearChildren();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }

        void Materialize(ThumbIndexRange range)
        {
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = panel;
            context.tiles = &tiles;
            context.thumbW = 64.0f;
            context.thumbH = 64.0f;
            context.model = &model;
            context.range = range;
            context.selectedIndex = selected;
            context.payloads = payloads;
            context.makeStableName = &StableName;
            controller.MaterializeTiles(context);
        }

        // Tiles match the model at their indices, in panel child order.
        bool TilesMatchModel() const
        {
            const auto& children = panel->ChildrenInOrder();
            if (children.size() != tiles.Tiles().size())
            {
                return false;
            }
            for (size_t k = 0; k < children.size(); ++k)
            {
                const ThumbTile& tile = tiles.Tiles()[k];
                const ThumbModelEntry& entry = model.At(tile.index);
                const wchar_t* prefix = (entry.kind == ThumbItemKind::Image) ? L"thumb_img"
                    : (entry.kind == ThumbItemKind::Up) ? L"nav_up" : L"nav_folder";
                if (tile.index != tiles.Range().first + k || children[k] != tile.focus ||
                    tile.kind != entry.kind || tile.focus->Name() != StableName(prefix, entry.path))
                {
                    return false;
                }
            }
            return true;
        }

        bool TileSelected(size_t index) const
        {
            const ThumbTile* tile = tiles.Find(index);
            if (tile == nullptr)
            {
                return false;
            }
            return tile->image ? tile->image->Selected() : tile->navTile->Selected();
        }

        // The normalized path an image tile of item `index` loads.
        std::wstring SourceOf(size_t index) const
        {
            return CommonUtil::NormalizePath(model.At(index).path.wstring());
        }

        size_t SelectedTileCount() const
        {
            size_t count = 0;
            for (const ThumbTile& tile : tiles.Tiles())
            {
                count += TileSelected(tile.index) ? 1 : 0;
            }
            return count;
        }
    };
}

FIC2_TEST(ThumbStripTiles_PanelPlacesPartialTilesAtTheirModelIndices)
{
    ThumbStripLayout layout;
    layout.Reset({ 100.0f, 100.0f, 150.0f, 50.0f, 100.0f, 80.0f, 120.0f, 100.0f }, 4.0f, 4.0f);
    ThumbTileSet tiles;
    auto panel = std::make_shared<ThumbStripPanel>();
    panel->SetSpacing(4.0f);
    panel->SetPadding(4.0f);
    panel->SetLayout(&layout);
    panel->SetTiles(&tiles);
    panel->SetRowHeight(64.0f);

    // Tiles for model indices 3..5 only: child 0 is item 3.
    std::vector<ThumbTile> bound;
    for (size_t index = 3; index < 6; ++index)
    {
        auto nav = std::make_shared<ThumbNavTile>(L"tile" + std::to_wstring(index));
        nav->SetFixedSize({ 64.0f, 64.0f });
        FIC2_REQUIRE(panel->AddChild(nav));
        bound.push_back({ index, ThumbItemKind::Folder, nav, nullptr, nav, nullptr });
    }
    tiles.Assign({ 3, 6 }, std::move(bound));
    FIC2_CHECK(tiles.Find(2) == nullptr);
    FIC2_CHECK(tiles.Find(6) == nullptr);
    FIC2_REQUIRE(tiles.Find(4) != nullptr);
    FIC2_CHECK(tiles.Find(4)->index == 4);

    // The panel spans the whole layout, not just its children.
    const FD2D::Size desired = panel->Measure({ 2000.0f, 200.0f });
    FIC2_CHECK(Near(desired.w, layout.ContentWidth()));
    FIC2_CHECK(Near(desired.h, 64.0f + 8.0f));

    panel->Arrange({ 10.0f, 0.0f, desired.w, desired.h });
    for (const ThumbTile& tile : tiles.Tiles())
    {
        const D2D1_RECT_F rect = tile.focus->LayoutRect();
        FIC2_CHECK(Near(rect.left, 10.0f + layout.Start(tile.index)));
        FIC2_CHECK(Near(rect.right - rect.left, layout.Extent(tile.index)));
        FIC2_CHECK(Near(rect.top, 4.0f));
        FIC2_CHECK(Near(rect.bottom, 4.0f + 64.0f));
    }
    FIC2_CHECK(Near(tiles.Tiles()[0].focus->LayoutRect().left, 10.0f + 366.0f));   // Start(3), not Start(0)

    // An extent change moves the tiles after it on the next arrange.
    layout.SetExtent(3, 10.0f);
    panel->Arrange({ 10.0f, 0.0f, layout.ContentWidth(), desired.h });
    FIC2_CHECK(Near(tiles.Find(5)->focus->LayoutRect().left, 10.0f + layout.Start(5)));
    FIC2_CHECK(Near(tiles.Find(4)->focus->LayoutRect().left, 10.0f + 380.0f));   // 366 + 10 + 4

    // Without any materialized tile the panel keeps the layout's extent and
    // the row height: the strip does not collapse to its padding.
    tiles.Clear();
    const FD2D::Size noTilesSize = panel->Measure({ 2000.0f, 200.0f });
    FIC2_CHECK(Near(noTilesSize.w, layout.ContentWidth()));
    FIC2_CHECK(Near(noTilesSize.h, 64.0f + 8.0f));
    panel->ClearChildren();

    // An empty strip without a row height measures as its padding.
    ThumbStripLayout emptyLayout;
    emptyLayout.Reset({}, 4.0f, 4.0f);
    ThumbTileSet noTiles;
    auto emptyPanel = std::make_shared<ThumbStripPanel>();
    emptyPanel->SetPadding(4.0f);
    emptyPanel->SetLayout(&emptyLayout);
    emptyPanel->SetTiles(&noTiles);
    const FD2D::Size emptySize = emptyPanel->Measure({ 100.0f, 100.0f });
    FIC2_CHECK(Near(emptySize.w, 8.0f));
    FIC2_CHECK(Near(emptySize.h, 8.0f));
}

FIC2_TEST(ThumbStripTiles_MaterializeBindsOnlyTheRange)
{
    Strip strip;
    FIC2_REQUIRE(strip.model.DisplayCount() == 15);

    strip.selected = 6;
    strip.Materialize({ 5, 9 });
    FIC2_CHECK((strip.tiles.Range() == ThumbIndexRange { 5, 9 }));
    FIC2_CHECK(strip.TilesMatchModel());
    FIC2_CHECK(strip.decodes.RequestCount() == 4);   // one thumbnail per bound image tile
    FIC2_CHECK(strip.TileSelected(6));
    FIC2_CHECK(strip.SelectedTileCount() == 1);

    // Moving the range rebinds only the new indices and drops the old ones.
    const auto kept7 = strip.tiles.Find(7)->focus;
    const auto kept8 = strip.tiles.Find(8)->focus;
    strip.Materialize({ 7, 11 });
    FIC2_CHECK(strip.TilesMatchModel());
    FIC2_CHECK(strip.tiles.Find(7)->focus == kept7);
    FIC2_CHECK(strip.tiles.Find(8)->focus == kept8);
    FIC2_CHECK(strip.tiles.Find(6) == nullptr);
    FIC2_CHECK(strip.decodes.RequestCount() == 6);
    FIC2_CHECK(strip.SelectedTileCount() == 0);   // the selection has no tile

    // Nav items bind too; a range is clamped to the model.
    strip.Materialize({ 0, 3 });
    FIC2_CHECK(strip.TilesMatchModel());
    FIC2_CHECK(strip.tiles.Find(0)->navTile != nullptr);
    strip.Materialize({ 13, 40 });
    FIC2_CHECK((strip.tiles.Range() == ThumbIndexRange { 13, 15 }));
    FIC2_CHECK(strip.TilesMatchModel());
}

FIC2_TEST(ThumbStripTiles_ProgressiveUpdateTouchesOnlyTheRange)
{
    Strip strip;
    strip.Materialize({ 7, 11 });
    const size_t requestsBefore = strip.decodes.RequestCount();

    // A progressive update inserts items before the range: the model commits,
    // the same index range shows shifted items, and nothing outside it gets a
    // tile or a decode request.
    FIC2_CHECK(strip.model.Stage({ File(L"a1.png"), File(L"a2.png"), File(L"zz.png") }) == 3);
    FIC2_CHECK(strip.model.Commit());
    FIC2_REQUIRE(strip.model.DisplayCount() == 18);
    const auto keptImg05 = strip.tiles.Find(8)->focus;   // img05 moves from index 8 to 10
    strip.Materialize({ 7, 11 });
    FIC2_CHECK(strip.TilesMatchModel());
    FIC2_CHECK(strip.panel->ChildrenInOrder().size() == 4);
    FIC2_CHECK(strip.tiles.Find(10)->focus == keptImg05);   // reordered, not recreated
    FIC2_CHECK(strip.decodes.RequestCount() == requestsBefore + 2);   // img02, img03 newly in range
}

FIC2_TEST(ThumbStripTiles_SelectionByModelIndexWithoutATile)
{
    Strip strip;
    strip.selected = 3;
    strip.Materialize({ 3, 6 });
    FIC2_REQUIRE(strip.TileSelected(3));

    size_t visibleCalls = 0;
    size_t visibleIndex = static_cast<size_t>(-1);
    size_t mainIndex = static_cast<size_t>(-1);
    std::wstring nonImage;
    ImageBrowserThumbStripController::SelectItemContext context {};
    context.model = &strip.model;
    context.tiles = &strip.tiles;
    context.selectedIndex = &strip.selected;
    context.ensureSelectionVisible = [&]()
    {
        ++visibleCalls;
        visibleIndex = strip.selected;
    };
    context.applyMainFromIndex = [&](size_t index)
    {
        mainIndex = index;
    };
    context.applyNonImageSelection = [&](const ThumbModelEntry& entry)
    {
        nonImage = entry.path.GetFilename();
    };

    // A far item (End, paging, type-to-select) has no tile: it is selected by
    // index, scrolled into view and shown; the old highlight is cleared.
    strip.controller.SelectItemByIndex(context, 12);
    FIC2_CHECK(strip.selected == 12);
    FIC2_CHECK(visibleCalls == 1 && visibleIndex == 12);
    FIC2_CHECK(mainIndex == 12);
    FIC2_CHECK(strip.SelectedTileCount() == 0);

    // Past the end clamps to the last item (End / page down).
    strip.controller.SelectItemByIndex(context, 999);
    FIC2_CHECK(strip.selected == 14);
    FIC2_CHECK(visibleCalls == 2 && visibleIndex == 14);
    FIC2_CHECK(mainIndex == 14);

    // Once its tile is bound, the selection shows.
    strip.Materialize({ 12, 15 });
    FIC2_CHECK(strip.TileSelected(14));
    FIC2_CHECK(strip.SelectedTileCount() == 1);

    // Home: the Up item, without a tile either; a non-image selection.
    strip.controller.SelectItemByIndex(context, 0);
    FIC2_CHECK(strip.selected == 0);
    FIC2_CHECK(visibleCalls == 3 && visibleIndex == 0);
    FIC2_CHECK(nonImage == kFolder.GetParent().GetFilename());
    FIC2_CHECK(strip.SelectedTileCount() == 0);   // tile 14 cleared

    // A materialized item is highlighted at once.
    strip.controller.SelectItemByIndex(context, 13);
    FIC2_CHECK(strip.TileSelected(13));
    FIC2_CHECK(strip.SelectedTileCount() == 1);
}

// Recycling releases tiles and their bitmaps, not the decoded payload: an item
// scrolled out of the materialized range and back is shown from the payload
// retained for it, without a new decode request.
FIC2_TEST(ThumbStripTiles_ScrollingBackToALoadedItemIssuesNoNewRequest)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(64, 64));
    Strip strip;

    strip.Materialize({ 3, 6 });
    FIC2_REQUIRE(strip.decodes.RequestCount() == 3);
    for (size_t index = 3; index < 6; ++index)
    {
        const ImageCore::ImageHandle handle = strip.decodes.LatestThumbnailFor(strip.SourceOf(index));
        FIC2_REQUIRE(handle != 0);
        strip.decodes.Complete(handle, S_OK, ThumbTestSupport::MakeBgraImage(4, 2));
        auto image = strip.tiles.Find(index)->image;
        FIC2_REQUIRE(SUCCEEDED(canvas.Render(*image)));   // presents (and retains) the payload
        FIC2_CHECK(image->GetBitmapSize().width == 4.0f);
    }
    FIC2_CHECK(strip.payloads->Count() == 3);

    // Out of the range: the tiles are released ...
    strip.Materialize({ 9, 12 });
    FIC2_CHECK(strip.tiles.Find(4) == nullptr);
    const size_t requestsAway = strip.decodes.RequestCount();
    FIC2_CHECK(requestsAway == 6);

    // ... and back: no new request; the next render rebuilds the bitmaps.
    strip.Materialize({ 3, 6 });
    FIC2_CHECK(strip.decodes.RequestCount() == requestsAway);
    for (size_t index = 3; index < 6; ++index)
    {
        FIC2_CHECK(strip.decodes.ThumbnailRequestsFor(strip.SourceOf(index)) == 1);
        auto image = strip.tiles.Find(index)->image;
        FIC2_REQUIRE(SUCCEEDED(canvas.Render(*image)));
        FIC2_CHECK(image->GetBitmapSize().width == 4.0f);
        FIC2_CHECK(image->GetBitmapSize().height == 2.0f);
    }
    FIC2_CHECK(strip.decodes.RequestCount() == requestsAway);

    // Another folder's listing drops them.
    strip.payloads->ResetForFolder(L"elsewhere");
    FIC2_CHECK(strip.payloads->Count() == 0);
}

// A tile that leaves the range is recycled: its queued request is canceled, it
// is rebound to another item under a new request generation, and the late
// completion of its previous request is never shown (or retained).
FIC2_TEST(ThumbStripTiles_RecycledTileNeverShowsItsPreviousItem)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(64, 64));
    Strip strip;

    strip.selected = 4;
    strip.Materialize({ 3, 7 });
    FIC2_REQUIRE(strip.decodes.RequestCount() == 4);
    std::vector<ImageCore::ImageHandle> oldHandles;
    std::vector<std::shared_ptr<FD2D::Wnd>> oldTiles;
    for (size_t index = 3; index < 7; ++index)
    {
        oldHandles.push_back(strip.decodes.LatestThumbnailFor(strip.SourceOf(index)));
        oldTiles.push_back(strip.tiles.Find(index)->focus);
    }

    // The range moves past them: every request is canceled and the same tile
    // windows are rebound to the new items.
    strip.Materialize({ 9, 13 });
    FIC2_CHECK(strip.TilesMatchModel());
    FIC2_CHECK(strip.controller.RecycledTileCount() == 0);
    const auto requests = strip.decodes.Requests();
    for (const ImageCore::ImageHandle handle : oldHandles)
    {
        FIC2_REQUIRE(handle != 0);
        FIC2_CHECK(requests[handle - 1].canceled);
    }
    for (size_t index = 9; index < 13; ++index)
    {
        const auto focus = strip.tiles.Find(index)->focus;
        FIC2_CHECK(std::find(oldTiles.begin(), oldTiles.end(), focus) != oldTiles.end());
        FIC2_CHECK(!strip.TileSelected(index));   // item 4's highlight did not travel
        FIC2_CHECK(strip.decodes.ThumbnailRequestsFor(strip.SourceOf(index)) == 1);
    }
    FIC2_CHECK(strip.decodes.OutstandingThumbnails() == 4);

    // The canceled requests complete anyway (a read that was already running):
    // nothing reaches the recycled tiles, and nothing is retained for the old items.
    for (const ImageCore::ImageHandle handle : oldHandles)
    {
        strip.decodes.Complete(handle, S_OK, ThumbTestSupport::MakeBgraImage(8, 8));
    }
    for (size_t index = 9; index < 13; ++index)
    {
        auto image = strip.tiles.Find(index)->image;
        FIC2_REQUIRE(SUCCEEDED(canvas.Render(*image)));
        FIC2_CHECK(image->GetBitmapSize().width == 0.0f);
    }
    for (size_t index = 3; index < 7; ++index)
    {
        FIC2_CHECK(strip.payloads->Find(strip.SourceOf(index)) == nullptr);
    }

    // The current request's result is shown.
    const ImageCore::ImageHandle current = strip.decodes.LatestThumbnailFor(strip.SourceOf(10));
    strip.decodes.Complete(current, S_OK, ThumbTestSupport::MakeBgraImage(4, 2));
    auto image = strip.tiles.Find(10)->image;
    FIC2_REQUIRE(SUCCEEDED(canvas.Render(*image)));
    FIC2_CHECK(image->GetBitmapSize().width == 4.0f);
    FIC2_CHECK(strip.payloads->Find(strip.SourceOf(10)) != nullptr);

    // Released tiles beyond what the range reuses are kept, up to the cap.
    strip.Materialize({ 14, 14 });
    FIC2_CHECK(strip.controller.RecycledTileCount() == 4);
    FIC2_CHECK(strip.controller.RecycledTileCount() <= ImageBrowserThumbStripController::kMaxRecycledTiles);
}

// Image tiles on screen request at Visible priority, the prefetch margin at
// Prefetch, so ImageCore serves on-screen thumbnails first (#35).
FIC2_TEST(ThumbStripTiles_VisibleTilesRequestAheadOfThePrefetchMargin)
{
    using ImageCore::RequestPriority;
    Strip strip;
    ImageBrowserThumbStripController::MaterializeContext context {};
    context.panel = strip.panel;
    context.tiles = &strip.tiles;
    context.thumbW = 64.0f;
    context.thumbH = 64.0f;
    context.model = &strip.model;
    context.range = { 3, 12 };
    context.visible = { 6, 9 };
    context.payloads = strip.payloads;
    context.makeStableName = &StableName;
    strip.controller.MaterializeTiles(context);

    auto priorityOf = [&](size_t index)
    {
        const auto requests = strip.decodes.Requests();
        const ImageCore::ImageHandle handle = strip.decodes.LatestThumbnailFor(strip.SourceOf(index));
        return requests.at(handle - 1).priority;
    };
    for (size_t index = 3; index < 12; ++index)
    {
        const bool visible = (index >= 6 && index < 9);
        FIC2_CHECK(priorityOf(index) == (visible ? RequestPriority::Visible : RequestPriority::Prefetch));
    }

    // The view moves: the bound tiles follow (queued requests are re-prioritized).
    context.range = { 4, 13 };
    context.visible = { 9, 11 };
    strip.controller.MaterializeTiles(context);
    FIC2_CHECK(priorityOf(6) == RequestPriority::Prefetch);
    FIC2_CHECK(priorityOf(9) == RequestPriority::Visible);
    FIC2_CHECK(priorityOf(10) == RequestPriority::Visible);
    FIC2_CHECK(priorityOf(12) == RequestPriority::Prefetch);
}

namespace
{
    // A strip whose materialized range is data-driven, as in the browser: the
    // layout is built from the model, and every view materializes
    // ThumbStripLayout::MaterializeRange of its viewport.
    struct VirtualStrip
    {
        static constexpr float kRow = 64.0f;
        static constexpr float kViewport = 1000.0f;

        ThumbTestSupport::RecordingDecodeService decodes {};
        std::shared_ptr<ThumbPayloadStore> payloads { std::make_shared<ThumbPayloadStore>() };
        ThumbItemModel model {};
        ThumbStripLayout layout {};
        ThumbTileSet tiles {};
        std::shared_ptr<ThumbStripPanel> panel { std::make_shared<ThumbStripPanel>() };
        ImageBrowserThumbStripController controller {};
        size_t views { 0 };
        size_t maxTiles { 0 };

        VirtualStrip()
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(&decodes);
            panel->SetLayout(&layout);
            panel->SetTiles(&tiles);
            panel->SetRowHeight(kRow);
        }

        ~VirtualStrip()
        {
            tiles.Clear();
            panel->ClearChildren();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }

        void Relayout()
        {
            std::vector<float> extents;
            extents.reserve(model.DisplayCount());
            for (size_t i = 0; i < model.DisplayCount(); ++i)
            {
                const ThumbModelEntry& entry = model.At(i);
                const float aspect = (entry.kind == ThumbItemKind::Image) ? model.AspectOf(entry.lookupKey) : 0.0f;
                extents.push_back(ThumbStripLayout::ImageExtent(kRow, aspect, 0.0f));
            }
            layout.Reset(std::move(extents), 4.0f, 4.0f);
        }

        // Shows [left, left + kViewport) and checks the bounds.
        void View(float left)
        {
            const auto [first, last] = layout.Range(left, left + kViewport);
            const auto [mFirst, mLast] = layout.MaterializeRange(left, left + kViewport);
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = panel;
            context.tiles = &tiles;
            context.thumbW = kRow;
            context.thumbH = kRow;
            context.model = &model;
            context.range = { mFirst, mLast };
            context.visible = { first, last };
            context.payloads = payloads;
            context.makeStableName = &StableName;
            controller.MaterializeTiles(context);

            const size_t visible = last - first;
            const size_t bound = visible + 2 * ThumbStripLayout::kMaxPrefetchItems;
            FIC2_CHECK(tiles.Range().first <= first && last <= tiles.Range().last);
            FIC2_CHECK(tiles.Range().Size() <= bound);
            FIC2_CHECK(panel->ChildrenInOrder().size() == tiles.Range().Size());
            FIC2_CHECK(decodes.OutstandingThumbnails() <= bound);
            FIC2_CHECK(controller.RecycledTileCount() <= ImageBrowserThumbStripController::kMaxRecycledTiles);
            maxTiles = (std::max)(maxTiles, tiles.Range().Size());
            ++views;
        }

        float LastLeft() const
        {
            return (std::max)(0.0f, layout.ContentWidth() - kViewport);
        }
    };

    std::wstring ImageName(size_t i)
    {
        wchar_t name[32] {};
        swprintf_s(name, L"img%05zu.png", i);
        return name;
    }
}

// A 10,000-image folder: materialized tiles and outstanding thumbnail requests
// stay within visible + 2 * M while the listing arrives progressively, while
// scrolling through it, and when jumping to either end - before and after the
// decoded aspects change the extents.
FIC2_TEST(ThumbStripTiles_TenThousandItemsStayBoundedWhileScrolling)
{
    constexpr size_t kImages = 10000;
    VirtualStrip strip;

    // Progressive listing: chunks arrive in arbitrary name order; each commit
    // rebuilds the layout and rebinds the view.
    strip.model.StageReset(kFolder, true, &IsTestImage);
    for (size_t chunk = 0; chunk < 10; ++chunk)
    {
        std::vector<Floar::VirtualFileEntry> entries;
        for (size_t i = chunk; i < kImages; i += 10)
        {
            entries.push_back(File(ImageName(i)));
        }
        (void)strip.model.Stage(entries);
        FIC2_CHECK(strip.model.Commit());
        strip.Relayout();
        strip.View(0.0f);
        strip.View(strip.LastLeft() * 0.5f);
    }
    FIC2_REQUIRE(strip.model.DisplayCount() == kImages + 1);   // Up + images

    // Scrolling through the folder, then jumping to the end and back.
    const float last = strip.LastLeft();
    for (float left = 0.0f; left < last; left += VirtualStrip::kViewport * 0.37f)
    {
        strip.View(left);
    }
    strip.View(last);
    FIC2_CHECK(strip.tiles.Find(kImages) != nullptr);   // the last item is bound
    strip.View(0.0f);
    FIC2_CHECK(strip.tiles.Find(0) != nullptr);

    // Decoded aspects arrive (wide and narrow images): the same bounds hold.
    for (size_t i = 1; i <= kImages; ++i)
    {
        const float aspect = (i % 3 == 0) ? 4.0f : ((i % 3 == 1) ? 0.25f : 1.5f);
        (void)strip.model.SetAspect(strip.model.At(i).lookupKey, aspect);
    }
    strip.Relayout();
    const float lastAfterAspects = strip.LastLeft();
    for (float left = 0.0f; left < lastAfterAspects; left += VirtualStrip::kViewport * 1.7f)
    {
        strip.View(left);
    }
    strip.View(lastAfterAspects);
    strip.View(0.0f);

    FIC2_CHECK(strip.views > 100);
    FIC2_CHECK(strip.maxTiles < 200);   // nowhere near the folder size
    std::printf("  %zu views, at most %zu tiles, %zu thumbnail requests in total\n",
        strip.views, strip.maxTiles, strip.decodes.RequestCount());
}
