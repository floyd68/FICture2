#pragma once

#include "FD2D/FD2D.h"

#include <memory>
#include <string>
#include <vector>

class ImageBrowserSplitCoordinator
{
public:
    static bool CanAddViewer(size_t paneCount, size_t maxViewers = 4);
    static size_t ResolveInsertIndex(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes,
        const std::wstring& afterName);
    static std::wstring NextSplitBrowserName();
    static std::wstring NextInsertBrowserName();

    static std::shared_ptr<FD2D::Wnd> BuildEqualWidthHostTree(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes);
};
