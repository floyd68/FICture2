#pragma once

#include "ImageBrowserMainPane.h"
#include "ImageBrowserThumbTypes.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace FD2D
{
    class ScrollView;
    class SplitPanel;
    class StackPanel;
    class Wnd;
}

class ImageBrowserThumbStripController
{
public:
    struct BuildResult
    {
        std::shared_ptr<FD2D::ScrollView> scroll {};
        std::shared_ptr<FD2D::StackPanel> panel {};
    };

    struct RebuildResult
    {
        size_t selectIndex { 0 };
        bool hasItems { false };
    };

    BuildResult Build(const std::shared_ptr<FD2D::SplitPanel>& rootSplit) const;

    RebuildResult RebuildList(
        const std::shared_ptr<FD2D::StackPanel>& panel,
        std::vector<ThumbItem>& items,
        float thumbW,
        float thumbH,
        bool showNavItems,
        const VirtualPath& currentFolder,
        const VirtualPath& preferSelectPath,
        const std::function<void(size_t)>& onSelectIndex,
        const std::function<void(size_t)>& onActivateIndex,
        const std::function<bool(const VirtualPath&, const VirtualPath&)>& pathEquals,
        const std::function<std::wstring(const wchar_t*, const VirtualPath&)>& makeStableName,
        const std::function<bool(const VirtualPath&)>& isSupportedImage,
        const std::vector<VirtualFileEntry>* preloadedEntries = nullptr) const;

    void SelectItemByIndex(
        std::vector<ThumbItem>& items,
        size_t& selectedIndex,
        std::shared_ptr<FD2D::Wnd>& selectedFocus,
        const std::shared_ptr<FD2D::ScrollView>& thumbScroll,
        const std::shared_ptr<FD2D::MainImage>& mainImage,
        std::wstring& mainPath,
        const VirtualPath& currentFolder,
        ImageBrowserMainPane* mainPane,
        bool syncSuppressBroadcast,
        size_t imageBrowserCount,
        const std::function<void(size_t)>& applyMainFromIndex,
        const std::function<void()>& refreshInfo,
        const std::function<void(const std::wstring&)>& publishFileName,
        size_t index) const;
};
