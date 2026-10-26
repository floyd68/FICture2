#include "TestHarness.h"

#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserThumbnailPane.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"

#include <cmath>
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

    // Counts thumbnail decode requests; never completes them.
    class CountingDecodeService : public IImageDecodeService
    {
    public:
        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest&,
            ImageCore::DecodedImageLoadCallback,
            ImageCore::RequestPriority) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return ++m_requests;
        }

        void Cancel(ImageCore::ImageHandle) override
        {
        }

        void SetPriority(ImageCore::ImageHandle, ImageCore::RequestPriority) override
        {
        }

        size_t RequestCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<size_t>(m_requests);
        }

    private:
        mutable std::mutex m_mutex;
        ImageCore::ImageHandle m_requests { 0 };
    };

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
        CountingDecodeService decodes {};
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
