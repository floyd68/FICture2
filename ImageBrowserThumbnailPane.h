#pragma once

#include "FD2D/FD2D.h"
#include "ImageBrowserThumbLayout.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace FD2D
{
    class ScrollView;
    class SplitPanel;
    class StackPanel;
}

// The strip's tile row: arranges its children (in order) at the positions of
// a ThumbStripLayout instead of measuring each tile. Falls back to plain
// StackPanel layout while the layout does not describe the children (for
// example during a rebuild).
class ThumbStripPanel : public FD2D::StackPanel
{
public:
    ThumbStripPanel();

    void SetLayout(const ThumbStripLayout* layout) { m_layout = layout; }

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;

private:
    bool LayoutMatchesChildren() const;

    const ThumbStripLayout* m_layout { nullptr };
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
