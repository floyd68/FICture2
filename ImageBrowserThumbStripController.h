#pragma once

#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbTypes.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace FD2D
{
    class StackPanel;
    class Wnd;
}

class ThumbPayloadCache;
class ThumbImageTile;

// Strip tiles over model indices. ThumbItemModel holds the items; only the
// materialized index range has tiles (ThumbTileSet), and selection is a model
// index that is applied to a tile when the tile is bound.
class ImageBrowserThumbStripController
{
public:
    struct MaterializeContext
    {
        std::shared_ptr<FD2D::StackPanel> panel {};
        ThumbTileSet* tiles { nullptr };
        float thumbW { 0.0f };
        float thumbH { 0.0f };
        // Classified, de-duplicated and ordered items (Up/folders shown when the
        // model was reset with nav items).
        const ThumbItemModel* model { nullptr };
        // Model indices to materialize (clamped to the model).
        ThumbIndexRange range {};
        // Model indices on screen: their image tiles load at Visible priority,
        // the rest of the range (the prefetch margin) at Prefetch.
        ThumbIndexRange visible {};
        // The selected model index: its tile is highlighted when bound.
        size_t selectedIndex { static_cast<size_t>(-1) };
        // Decoded payloads (bounded CPU cache): an image tile bound to an item
        // with a cached payload shows it without a new decode request.
        std::shared_ptr<ThumbPayloadCache> payloads {};
        std::function<void(size_t)> onSelectIndex {};
        std::function<void(size_t)> onActivateIndex {};
        // An image tile's decoded aspect ratio became known (lookup key, aspect).
        std::function<void(const std::wstring&, float)> onImageAspect {};
        std::function<std::wstring(const wchar_t*, const Floar::VirtualPath&)> makeStableName {};
    };

    struct SelectItemContext
    {
        const ThumbItemModel* model { nullptr };
        const ThumbTileSet* tiles { nullptr };
        size_t* selectedIndex { nullptr };
        // Scrolls the selected model index into view (no tile needed).
        std::function<void()> ensureSelectionVisible {};
        bool syncSuppressBroadcast { false };
        size_t imageBrowserCount { 0 };
        std::function<void(size_t)> applyMainFromIndex {};
        std::function<void(const ThumbModelEntry&)> applyNonImageSelection {};
        std::function<void(const std::wstring&)> publishFileName {};
    };

    // Binds tiles for the model indices of `context.range`: releases the tiles
    // outside the range, then creates missing tiles (reusing released image
    // tiles), and updates and reorders the existing ones. Work is proportional
    // to the range (and the tiles it replaces), not to the model size.
    //
    // A released image tile's request is canceled, and its bitmap and its pin
    // on the payload are dropped; the tile is kept for reuse (up to
    // kMaxRecycledTiles) and rebound to another item under a new request
    // generation, so the late completion of its previous request is never
    // shown. Decoded payloads stay in the payload cache, within its bounds.
    // A tile that stays bound to its item is told the item's listed identity,
    // so a file replaced at the same path is decoded again.
    void MaterializeTiles(const MaterializeContext& context);

    // Selects model item `index` (clamped to the model) whether or not it has
    // a tile; a materialized tile shows the highlight at once, any other when
    // it is bound.
    void SelectItemByIndex(const SelectItemContext& context, size_t index) const;

    static void SetTileSelected(const ThumbTile& tile, bool selected);

    // Admits every bound image tile's completed decode into the payload cache
    // (ImageBrowserThumbImage::AdmitCompletedPayload), rendered or not, so
    // decoded payloads are accounted from the first frame after they arrive.
    static void AdmitCompletedPayloads(const ThumbTileSet& tiles);

    // Released image tiles kept for reuse, at most this many.
    static constexpr size_t kMaxRecycledTiles = 32;
    size_t RecycledTileCount() const { return m_recycled.size(); }

private:
    std::vector<std::shared_ptr<ThumbImageTile>> m_recycled {};
};
