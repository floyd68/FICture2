#pragma once

#include "FD2D/FD2D.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace FD2D
{
    class ScrollView;
    class SplitPanel;
    class StackPanel;
}

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
    bool TryGetStripRect(D2D1_RECT_F& outRect) const;
    size_t PagingStep(float itemExtent) const;

    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
    std::shared_ptr<FD2D::StackPanel> m_thumbPanel {};
    std::function<void(int)> m_onWheelSteps {};
    std::function<void()> m_onWheelFocus {};
    int m_wheelRemainder { 0 };
};
