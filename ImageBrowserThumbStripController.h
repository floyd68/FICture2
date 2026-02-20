#pragma once

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
    class MainImage;
    class Wnd;
}

class ImageBrowserThumbStripController
{
public:
    struct RebuildResult
    {
        size_t selectIndex { 0 };
        bool hasItems { false };
    };

    struct RebuildListContext
    {
        std::shared_ptr<FD2D::StackPanel> panel {};
        std::vector<ThumbItem>* items { nullptr };
        float thumbW { 0.0f };
        float thumbH { 0.0f };
        bool showNavItems { false };
        VirtualPath currentFolder {};
        VirtualPath preferSelectPath {};
        std::function<void(size_t)> onSelectIndex {};
        std::function<void(size_t)> onActivateIndex {};
        std::function<bool(const VirtualPath&, const VirtualPath&)> pathEquals {};
        std::function<std::wstring(const wchar_t*, const VirtualPath&)> makeStableName {};
        std::function<bool(const VirtualPath&)> isSupportedImage {};
        const std::vector<VirtualFileEntry>* preloadedEntries { nullptr };
    };

    struct SelectItemContext
    {
        std::vector<ThumbItem>* items { nullptr };
        size_t* selectedIndex { nullptr };
        std::shared_ptr<FD2D::Wnd>* selectedFocus { nullptr };
        std::function<void()> ensureSelectionVisible {};
        bool syncSuppressBroadcast { false };
        size_t imageBrowserCount { 0 };
        std::function<void(size_t)> applyMainFromIndex {};
        std::function<void(const ThumbItem&)> applyNonImageSelection {};
        std::function<void(const std::wstring&)> publishFileName {};
    };

    RebuildResult RebuildList(const RebuildListContext& context) const;

    void SelectItemByIndex(const SelectItemContext& context, size_t index) const;
};
