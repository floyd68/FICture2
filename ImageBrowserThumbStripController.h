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

class ThumbPayloadStore;

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
        // The selected model index: its tile is highlighted when bound.
        size_t selectedIndex { static_cast<size_t>(-1) };
        // Decoded payloads retained per item: an image tile bound to an item
        // with a retained payload shows it without a new decode request.
        std::shared_ptr<ThumbPayloadStore> payloads {};
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

    // Binds tiles for the model indices of `context.range`: creates missing
    // tiles, updates and reorders the existing ones, and removes tiles outside
    // the range. Work is proportional to the range (and the tiles it replaces),
    // not to the model size.
    void MaterializeTiles(const MaterializeContext& context) const;

    // Selects model item `index` (clamped to the model) whether or not it has
    // a tile; a materialized tile shows the highlight at once, any other when
    // it is bound.
    void SelectItemByIndex(const SelectItemContext& context, size_t index) const;

    static void SetTileSelected(const ThumbTile& tile, bool selected);
};
