#pragma once

#include "FD2D/FD2D.h"
#include "FD2D/MainImage.h"

#include <functional>
#include <memory>
#include <string>

class InfoBar;
class PathBar;

class ImageBrowserMainPane
{
public:
    void Build(
        const std::shared_ptr<FD2D::SplitPanel>& rootSplit,
        const std::wstring& initialFile,
        const std::function<void(const FD2D::Image::ViewTransform&)>& onViewChanged,
        const std::function<void()>& onClick,
        const std::function<void(FD2D::MainImage&)>& applyIni);

    std::shared_ptr<FD2D::MainImage> MainImage() const { return m_mainImage; }

    void UpdateInfo(
        const std::wstring& pathText,
        const std::wstring& infoText,
        const std::wstring& zoomText);

    void ResetInfoCache();

private:
    std::shared_ptr<FD2D::DockPanel> m_mainDock {};
    std::shared_ptr<PathBar> m_pathBar {};
    std::shared_ptr<InfoBar> m_infoBar {};
    std::shared_ptr<FD2D::MainImage> m_mainImage {};

    std::wstring m_lastPathText {};
    std::wstring m_lastInfoText {};
    std::wstring m_lastZoomText {};
};
