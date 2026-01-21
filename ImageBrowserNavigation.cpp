#include "ImageBrowserNavigation.h"

bool ImageBrowserNavigation::NavigateUp(
    const VirtualPath& currentFolder,
    const std::function<void(const VirtualPath&)>& onNavigateToFolder) const
{
    if (currentFolder.empty())
    {
        return false;
    }

    const VirtualPath parent = currentFolder.GetParent();
    if (parent == currentFolder)
    {
        return false;
    }

    if (onNavigateToFolder)
    {
        onNavigateToFolder(parent);
    }
    return true;
}

bool ImageBrowserNavigation::NavigateToFolder(
    const VirtualPath& folder,
    const std::function<bool(const VirtualPath&)>& isDirectory,
    VirtualPath& currentFolder,
    const std::function<void(const VirtualPath&)>& rebuildThumbList,
    const std::function<void()>& resetThumbScroll,
    const std::function<void(size_t)>& selectIndex,
    const std::vector<ThumbItem>& items) const
{
    if (isDirectory && !isDirectory(folder))
    {
        return false;
    }

    currentFolder = folder;
    if (rebuildThumbList)
    {
        rebuildThumbList(VirtualPath());
    }

    if (resetThumbScroll)
    {
        resetThumbScroll();
    }

    if (selectIndex)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].kind == ThumbItemKind::Image)
            {
                selectIndex(i);
                break;
            }
        }
    }

    return true;
}

bool ImageBrowserNavigation::NavigateToFile(
    const VirtualPath& filePath,
    const std::function<bool(const VirtualPath&)>& fileExists,
    const std::function<bool(const std::wstring&)>& isSupportedPath,
    const std::function<bool(const VirtualPath&)>& isDirectory,
    VirtualPath& currentFolder,
    const std::function<void(const VirtualPath&)>& rebuildThumbList) const
{
    if (fileExists && !fileExists(filePath))
    {
        return false;
    }

    if (isSupportedPath && !isSupportedPath(filePath.GetDisplayPath()))
    {
        return false;
    }

    const VirtualPath folder = filePath.GetParent();
    if (isDirectory && !isDirectory(folder))
    {
        return false;
    }

    currentFolder = folder;
    if (rebuildThumbList)
    {
        rebuildThumbList(filePath);
    }
    return true;
}

bool ImageBrowserNavigation::ActivateSelected(
    size_t selectedIndex,
    const std::vector<ThumbItem>& items,
    const std::function<void(size_t)>& applyMainFromIndex,
    const std::function<void(const VirtualPath&)>& onNavigateToFolder) const
{
    if (selectedIndex >= items.size())
    {
        return false;
    }

    const ThumbItem item = items[selectedIndex];
    if (item.kind == ThumbItemKind::Image)
    {
        if (applyMainFromIndex)
        {
            applyMainFromIndex(selectedIndex);
        }
        return true;
    }

    if (item.kind == ThumbItemKind::Up || item.kind == ThumbItemKind::Folder)
    {
        if (onNavigateToFolder)
        {
            onNavigateToFolder(item.path);
        }
        return true;
    }

    return false;
}
