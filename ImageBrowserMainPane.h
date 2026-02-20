#pragma once

#include "FD2D/FD2D.h"
#include "FD2D/MainImage.h"

#include <functional>
#include <memory>
#include <string>
#include <wrl/client.h>

class InfoBar;
class PathBar;

class ImageBrowserMainPane : public FD2D::Wnd
{
public:
    ImageBrowserMainPane();

    void Build(
        const std::shared_ptr<FD2D::SplitPanel>& rootSplit,
        const std::wstring& initialFile,
        const std::function<void(const FD2D::Image::ViewTransform&)>& onViewChanged,
        const std::function<void()>& onClick,
        const std::function<void(FD2D::MainImage&)>& applyIni,
        const std::function<bool(const POINT&)>& onContextMenuRequest,
        const std::function<void()>& onMainImageWheelFocus);

    std::shared_ptr<FD2D::MainImage> MainImage() const { return m_mainImage; }
    bool TryGetMainImageRect(D2D1_RECT_F& outRect) const;
    bool ContainsMainPoint(const POINT& pt) const;
    bool TryGetMainRectForPoint(const POINT& pt, D2D1_RECT_F& outRect) const;
    void PauseMainImageViewAnimation();
    void RenderMainBackgroundD3D(
        FD2D::Backplate* backplate,
        bool hasFocus,
        const D2D1_COLOR_F& baseBackground,
        const D2D1_COLOR_F& focusedBackground);
    void RenderMainBackgroundD2D(
        ID2D1RenderTarget* target,
        bool d3dActive,
        bool hasFocus,
        const D2D1_COLOR_F& baseBackground,
        const D2D1_COLOR_F& focusedBackground);
    void RenderCenteredMainOverlayBitmap(
        ID2D1RenderTarget* target,
        ID2D1Bitmap* bitmap,
        float sizeFactor = 0.30f);
    void RenderOnMainRect(const std::function<void(const D2D1_RECT_F&)>& renderer);

    void UpdateInfo(
        const std::wstring& pathText,
        const std::wstring& infoText,
        const std::wstring& zoomText);

    void ResetInfoCache();
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    std::shared_ptr<FD2D::DockPanel> m_mainDock {};
    std::shared_ptr<PathBar> m_pathBar {};
    std::shared_ptr<InfoBar> m_infoBar {};
    std::shared_ptr<FD2D::MainImage> m_mainImage {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_mainBackgroundBrush {};
    std::function<bool(const POINT&)> m_onContextMenuRequest {};
    std::function<void()> m_onMainImageWheelFocus {};

    std::wstring m_lastPathText {};
    std::wstring m_lastInfoText {};
    std::wstring m_lastZoomText {};
};
