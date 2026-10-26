#pragma once

#include "VirtualPath.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace FD2D
{
    class Wnd;
}

class ThumbNavTile;
class ThumbImageTile;
class ImageBrowserThumbImage;

enum class ThumbItemKind
{
    Up,
    Folder,
    Image
};

// Model (display) indices [first, last).
struct ThumbIndexRange
{
    size_t first { 0 };
    size_t last { 0 };

    bool Contains(size_t index) const { return index >= first && index < last; }
    size_t Size() const { return (last > first) ? last - first : 0; }
    bool operator==(const ThumbIndexRange&) const = default;
};

// The UI objects that show one model item. The item itself (kind, path, key)
// is ThumbItemModel::At(index).
struct ThumbTile
{
    size_t index { 0 };
    ThumbItemKind kind { ThumbItemKind::Image };
    std::shared_ptr<FD2D::Wnd> focus {};   // the tile window
    std::shared_ptr<ImageBrowserThumbImage> image {};
    std::shared_ptr<ThumbNavTile> navTile {};
    std::shared_ptr<ThumbImageTile> imageTile {};
};

// The materialized tiles: one per model index in Range(), in index order.
// Items outside the range have no tile; per-item strip paths work from model
// indices and touch only these tiles.
class ThumbTileSet
{
public:
    const ThumbIndexRange& Range() const { return m_range; }
    const std::vector<ThumbTile>& Tiles() const { return m_tiles; }

    // The tile of model item `index`, or nullptr when it is not materialized.
    const ThumbTile* Find(size_t index) const
    {
        return m_range.Contains(index) ? &m_tiles[index - m_range.first] : nullptr;
    }

    // `tiles` holds one tile per index of `range`, in order.
    void Assign(ThumbIndexRange range, std::vector<ThumbTile> tiles)
    {
        m_range = range;
        m_tiles = std::move(tiles);
        if (m_tiles.size() != m_range.Size())
        {
            m_range.last = m_range.first + m_tiles.size();
        }
    }

    void Clear()
    {
        m_range = {};
        m_tiles.clear();
    }

private:
    ThumbIndexRange m_range {};
    std::vector<ThumbTile> m_tiles {};
};
