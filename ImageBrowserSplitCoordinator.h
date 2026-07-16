#pragma once

#include "FD2D/FD2D.h"

#include <memory>
#include <string>
#include <vector>

class ImageBrowserSplitCoordinator
{
public:
    // Matches NIFDiff's compare grid: 1-4 in one row, 5-8 as a two-row layout.
    static constexpr size_t kMaxViewers = 8;

    static bool CanAddViewer(size_t paneCount, size_t maxViewers = kMaxViewers);
    static size_t ResolveInsertIndex(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes,
        const std::wstring& afterName);
    static std::wstring NextSplitBrowserName();
    static std::wstring NextInsertBrowserName();

    // Nested SplitPanel host tree:
    //   1-4 panes -> one row of equal widths (n x 1)
    //   5-6 panes -> two rows, 3 columns (3 x 2)
    //   7-8 panes -> two rows, 4 columns (4 x 2)
    static std::shared_ptr<FD2D::Wnd> BuildEqualWidthHostTree(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes);
};
