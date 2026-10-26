#pragma once

#include "FD2D/FD2D.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbTypes.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace FD2D
{
    class ScrollView;
    class SplitPanel;
    class StackPanel;
}

// The strip's tile row: its width is the ThumbStripLayout content width and
// its height the row height it is given (not measured from its tiles, which
// may cover any subset of the model or none), and each materialized tile is
// placed at its model index's layout position (not by its child position). A
// tile whose index the layout does not describe is not arranged.
class ThumbStripPanel : public FD2D::StackPanel
{
public:
    ThumbStripPanel();

    void SetLayout(const ThumbStripLayout* layout) { m_layout = layout; }
    // The tiles to place; their windows are this panel's children.
    void SetTiles(const ThumbTileSet* tiles) { m_tiles = tiles; }
    const ThumbStripLayout* Layout() const { return m_layout; }
    const ThumbTileSet* Tiles() const { return m_tiles; }
    // Height of the tile row (the thumbnail height), without padding.
    void SetRowHeight(float height) { m_rowHeight = height; }
    float RowHeight() const { return m_rowHeight; }

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;
    // Places the tiles again within the bounds of the last Arrange, for a tile
    // set that changed after the layout pass.
    void ArrangeTiles() { Arrange(m_bounds); }

private:
    const ThumbStripLayout* m_layout { nullptr };
    const ThumbTileSet* m_tiles { nullptr };
    float m_rowHeight { 0.0f };
};

class ImageBrowserThumbnailPane : public FD2D::Wnd
{
public:
    ImageBrowserThumbnailPane();

    void Build(
        const std::shared_ptr<FD2D::SplitPanel>& rootSplit,
        const std::function<void(int)>& onWheelSteps,
        const std::function<void()>& onWheelFocus);

    std::shared_ptr<FD2D::ScrollView> Scroll() const { return m_thumbScroll; }
    std::shared_ptr<FD2D::StackPanel> Panel() const { return m_thumbPanel; }
    std::shared_ptr<ThumbStripPanel> StripPanel() const { return m_thumbPanel; }
    // Panel padding and item spacing that ThumbStripLayout must use.
    static constexpr float kPanelPadding = 4.0f;
    static constexpr float kItemSpacing = 4.0f;
    bool TryGetStripRect(D2D1_RECT_F& outRect) const;
    size_t PagingStep(float itemExtent) const;

    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
    std::shared_ptr<ThumbStripPanel> m_thumbPanel {};
    std::function<void(int)> m_onWheelSteps {};
    std::function<void()> m_onWheelFocus {};
    int m_wheelRemainder { 0 };
};
