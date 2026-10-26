#include "ImageBrowserThumbStripController.h"

#include "ArchiveBadge.h"
#include "DeferredRenderScope.h"
#include "ArchiveTypes.h"
#include "CommonUtil.h"
#include "AppLog.h"
#include "FD2D/FD2D.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "VirtualFileSystem.h"
#include "ArchiveReader.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <unordered_set>

namespace
{
    std::wstring GetArchiveBadgeText(const Floar::VirtualPath& path)
    {
        const std::wstring ext = CommonUtil::ToLower(path.hostPath.extension().wstring());
        return ArchiveBadge::BadgeLabelForExt(ext);
    }

    ThumbNavTile::IconTint GetArchiveIconTint(const Floar::VirtualPath& path)
    {
        const std::wstring ext = CommonUtil::ToLower(path.hostPath.extension().wstring());
        if (Floar::ArchiveTypes::IsBethesdaArchiveExt(ext))
        {
            return ThumbNavTile::IconTint::ArchiveBlue;
        }

        if (Floar::ArchiveTypes::IsCommonArchiveExt(ext))
        {
            return ThumbNavTile::IconTint::ArchiveRed;
        }

        return ThumbNavTile::IconTint::None;
    }
}

void ImageBrowserThumbStripController::SetTileSelected(const ThumbTile& tile, bool selected)
{
    if (tile.image)
    {
        tile.image->SetSelected(selected);
    }
    if (tile.navTile)
    {
        tile.navTile->SetSelected(selected);
    }
}

void ImageBrowserThumbStripController::AdmitCompletedPayloads(const ThumbTileSet& tiles)
{
    for (const ThumbTile& tile : tiles.Tiles())
    {
        if (tile.image)
        {
            (void)tile.image->AdmitCompletedPayload();
        }
    }
}

void ImageBrowserThumbStripController::MaterializeTiles(const MaterializeContext& context)
{
    if (!context.panel || context.tiles == nullptr || context.model == nullptr)
    {
        return;
    }

    const auto t_materialize = std::chrono::steady_clock::now();

    // Attached tiles' setters, removals and the selection render one frame
    // in all (or none, inside an enclosing scope or a render).
    const DeferredRenderScope batchRender(context.backplate);

    const auto& panel = context.panel;
    const ThumbItemModel& model = *context.model;
    const float thumbW = context.thumbW;
    const float thumbH = context.thumbH;
    const auto& onSelectIndex = context.onSelectIndex;
    const auto& onActivateIndex = context.onActivateIndex;
    const auto& makeStableName = context.makeStableName;

    const size_t count = model.DisplayCount();
    ThumbIndexRange range { (std::min)(context.range.first, count), (std::min)(context.range.last, count) };
    range.last = (std::max)(range.first, range.last);

    // Child names are stable per item path, so a tile that stays in the range
    // keeps its window.
    std::vector<std::wstring> desiredOrder;
    desiredOrder.reserve(range.Size());
    for (size_t index = range.first; index < range.last; ++index)
    {
        const ThumbModelEntry& entry = model.At(index);
        const wchar_t* prefix = (entry.kind == ThumbItemKind::Image) ? L"thumb_img"
            : (entry.kind == ThumbItemKind::Up) ? L"nav_up" : L"nav_folder";
        desiredOrder.push_back(makeStableName ? makeStableName(prefix, entry.path) : std::wstring(prefix));
    }
    const std::unordered_set<std::wstring> desiredNames(desiredOrder.begin(), desiredOrder.end());

    auto getExistingChild = [panel](const std::wstring& name) -> std::shared_ptr<FD2D::Wnd>
    {
        const auto& children = panel->Children();
        auto it = children.find(name);
        if (it == children.end())
        {
            return nullptr;
        }
        return it->second;
    };

    // Release the tiles that left the range first, so their image tiles can be
    // reused below. Releasing cancels a request still queued or running.
    {
        std::vector<std::wstring> toRemove;
        for (const auto& kv : panel->Children())
        {
            if (desiredNames.find(kv.first) == desiredNames.end())
            {
                toRemove.push_back(kv.first);
            }
        }
        for (const auto& name : toRemove)
        {
            auto imageTile = std::dynamic_pointer_cast<ThumbImageTile>(getExistingChild(name));
            (void)panel->RemoveChild(name);
            if (imageTile)
            {
                imageTile->Recycle();
                if (m_recycled.size() < kMaxRecycledTiles)
                {
                    m_recycled.push_back(std::move(imageTile));
                }
            }
        }
    }

    std::vector<ThumbTile> tiles;
    tiles.reserve(range.Size());

    for (size_t index = range.first; index < range.last; ++index)
    {
        const ThumbModelEntry& entry = model.At(index);
        const std::wstring& name = desiredOrder[index - range.first];
        ThumbTile bound {};
        bound.index = index;
        bound.kind = entry.kind;

        if (entry.kind == ThumbItemKind::Image)
        {
            const Floar::VirtualPath& p = entry.path;
            const auto priority = context.visible.Contains(index)
                ? ImageCore::RequestPriority::Visible
                : ImageCore::RequestPriority::Prefetch;
            auto thumbTile = std::dynamic_pointer_cast<ThumbImageTile>(getExistingChild(name));
            if (!thumbTile)
            {
                (void)panel->RemoveChild(name);
                if (!m_recycled.empty())
                {
                    thumbTile = std::move(m_recycled.back());
                    m_recycled.pop_back();
                    thumbTile->SetName(name);
                }
                else
                {
                    thumbTile = std::make_shared<ThumbImageTile>(name);
                }
                // Set before the source, so the request goes out at this priority
                // (or not at all, when a cached payload is shown).
                thumbTile->ImageWnd()->SetPayloadCache(context.payloads);
                thumbTile->ImageWnd()->SetLoadPriority(priority);
                thumbTile->SetFixedHeight(thumbH);
                thumbTile->SetSourceFile(p.wstring(), entry.identity);
                thumbTile->SetCaption(p.filename().wstring());
                (void)panel->AddChild(thumbTile);
            }
            else
            {
                thumbTile->ImageWnd()->SetLoadPriority(priority);
                thumbTile->SetFixedHeight(thumbH);
                thumbTile->ImageWnd()->SetSourceIdentity(entry.identity);
            }

            thumbTile->SetOnClick([onSelectIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
            });
            thumbTile->SetOnAspectChanged([onImageAspect = context.onImageAspect, key = entry.lookupKey](float aspect)
            {
                if (onImageAspect)
                {
                    onImageAspect(key, aspect);
                }
            });

            bound.focus = thumbTile;
            bound.image = thumbTile->ImageWnd();
            bound.imageTile = thumbTile;
        }
        else
        {
            const bool isUp = (entry.kind == ThumbItemKind::Up);
            const Floar::VirtualPath& dir = entry.path;
            auto tile = std::dynamic_pointer_cast<ThumbNavTile>(getExistingChild(name));
            // A new tile is configured before it is attached, so its setters
            // invalidate nothing; an attached tile is invalidated only by the
            // values that change.
            bool attach = false;
            if (!tile)
            {
                (void)panel->RemoveChild(name);
                tile = std::make_shared<ThumbNavTile>(name);
                attach = true;
            }

            tile->SetFixedSize({ thumbW, thumbH });
            tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
            if (isUp)
            {
                tile->SetText(L"..");
                tile->SetIcon(ThumbNavTile::IconKind::Up);
            }
            else
            {
                tile->SetText(dir.filename().wstring());
                tile->SetIcon(ThumbNavTile::IconKind::Folder);
                if (entry.isArchive)
                {
                    tile->SetBadgeText(GetArchiveBadgeText(dir));
                    tile->SetIconTint(GetArchiveIconTint(dir));
                }
                else
                {
                    tile->SetBadgeText(L"");
                    tile->SetIconTint(ThumbNavTile::IconTint::None);
                }
            }
            tile->SetOnClick([onSelectIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
            });
            tile->SetOnDoubleClick([onSelectIndex, onActivateIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
                if (onActivateIndex)
                {
                    onActivateIndex(index);
                }
            });
            if (attach)
            {
                (void)panel->AddChild(tile);
            }

            bound.focus = tile;
            bound.navTile = tile;
        }

        // Selection is model state: a tile shows it when it is bound.
        SetTileSelected(bound, index == context.selectedIndex);

        tiles.push_back(std::move(bound));
    }

    (void)panel->ReorderChildren(desiredOrder);
    context.tiles->Assign(range, std::move(tiles));

    {
        const auto materializeMs = FIC2_ELAPSED_MS(t_materialize);
        if (materializeMs > 50)
        {
            FIC2_LOG_INFO("[UI stall] ThumbStrip::MaterializeTiles took {}ms ({} tiles of {} items)",
                materializeMs, range.Size(), count);
        }
    }
}

void ImageBrowserThumbStripController::SelectItemByIndex(const SelectItemContext& context, size_t index) const
{
    if (context.model == nullptr || context.selectedIndex == nullptr)
    {
        return;
    }

    const ThumbItemModel& model = *context.model;
    auto& selectedIndex = *context.selectedIndex;
    const auto& ensureSelectionVisible = context.ensureSelectionVisible;
    const bool syncSuppressBroadcast = context.syncSuppressBroadcast;
    const size_t imageBrowserCount = context.imageBrowserCount;
    const auto& applyMainFromIndex = context.applyMainFromIndex;
    const auto& applyNonImageSelection = context.applyNonImageSelection;
    const auto& publishFileName = context.publishFileName;

    const size_t count = model.DisplayCount();
    if (count == 0)
    {
        return;
    }

    if (index >= count)
    {
        index = count - 1;
    }

    if (context.tiles != nullptr)
    {
        if (const ThumbTile* previous = context.tiles->Find(selectedIndex))
        {
            SetTileSelected(*previous, false);
        }
    }

    selectedIndex = index;

    if (context.tiles != nullptr)
    {
        if (const ThumbTile* tile = context.tiles->Find(index))
        {
            SetTileSelected(*tile, true);
        }
    }

    // Scrolling works from the model index; the item need not have a tile.
    if (ensureSelectionVisible)
    {
        ensureSelectionVisible();
    }

    const ThumbModelEntry& selected = model.At(selectedIndex);
    if (selected.kind == ThumbItemKind::Image)
    {
        if (applyMainFromIndex)
        {
            applyMainFromIndex(selectedIndex);
        }
    }
    else if (selected.kind == ThumbItemKind::Folder || selected.kind == ThumbItemKind::Up)
    {
        if (applyNonImageSelection)
        {
            applyNonImageSelection(selected);
        }
    }

    // Folder compare sync:
    // If we are in compare mode (2+ ImageBrowsers) and a new image is selected in the thumbnail list,
    // propagate its filename to other ImageBrowsers. Receivers select the same filename in their
    // current directory (if present).
    if (!syncSuppressBroadcast && imageBrowserCount >= 2 && selectedIndex < model.DisplayCount())
    {
        const ThumbModelEntry& item = model.At(selectedIndex);
        if (item.kind == ThumbItemKind::Image)
        {
            const std::wstring fileNameLower = CommonUtil::ToLower(item.path.filename().wstring());
            if (!fileNameLower.empty() && publishFileName)
            {
                publishFileName(fileNameLower);
            }
        }
    }
}
