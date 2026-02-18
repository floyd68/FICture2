#pragma once

#include "ImageBrowserThumbTypes.h"
#include "VirtualPath.h"

#include <functional>
#include <string>
#include <vector>

class ImageBrowserNavigation
{
public:
    bool NavigateUp(
        const VirtualPath& currentFolder,
        const std::function<void(const VirtualPath&)>& onNavigateToFolder) const;

    bool NavigateToFolder(
        const VirtualPath& folder,
        const std::function<bool(const VirtualPath&)>& isDirectory,
        VirtualPath& currentFolder,
        const std::function<void(const VirtualPath&)>& rebuildThumbList,
        const std::function<void()>& resetThumbScroll) const;

    bool NavigateToFile(
        const VirtualPath& filePath,
        const std::function<bool(const VirtualPath&)>& fileExists,
        const std::function<bool(const std::wstring&)>& isSupportedPath,
        const std::function<bool(const VirtualPath&)>& isDirectory,
        VirtualPath& currentFolder,
        const std::function<void(const VirtualPath&)>& rebuildThumbList) const;

    bool ActivateSelected(
        size_t selectedIndex,
        const std::vector<ThumbItem>& items,
        const std::function<void(size_t)>& applyMainFromIndex,
        const std::function<void(const VirtualPath&)>& onNavigateToFolder) const;
};
