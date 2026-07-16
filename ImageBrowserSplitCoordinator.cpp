#include "ImageBrowserSplitCoordinator.h"

#include <functional>
#include <string>

bool ImageBrowserSplitCoordinator::CanAddViewer(size_t paneCount, size_t maxViewers)
{
    return paneCount < maxViewers;
}

size_t ImageBrowserSplitCoordinator::ResolveInsertIndex(
    const std::vector<std::shared_ptr<FD2D::Wnd>>& panes,
    const std::wstring& afterName)
{
    size_t insertIndex = panes.size();
    for (size_t i = 0; i < panes.size(); ++i)
    {
        if (panes[i] && panes[i]->Name() == afterName)
        {
            insertIndex = i + 1;
            break;
        }
    }

    if (insertIndex > panes.size())
    {
        insertIndex = panes.size();
    }
    return insertIndex;
}

std::wstring ImageBrowserSplitCoordinator::NextSplitBrowserName()
{
    static int s_splitId = 1;
    return L"browser_split_" + std::to_wstring(s_splitId++);
}

std::wstring ImageBrowserSplitCoordinator::NextInsertBrowserName()
{
    static int s_insertId = 20001;
    return L"browser_insert_" + std::to_wstring(s_insertId++);
}

std::shared_ptr<FD2D::Wnd> ImageBrowserSplitCoordinator::BuildEqualWidthHostTree(
    const std::vector<std::shared_ptr<FD2D::Wnd>>& panes)
{
    const size_t n = panes.size();
    if (n == 0)
    {
        return nullptr;
    }

    static int s_hostId = 1;

    auto makeSplit = [](
        const std::wstring& name,
        FD2D::SplitterOrientation orientation,
        float ratio,
        const std::shared_ptr<FD2D::Wnd>& a,
        const std::shared_ptr<FD2D::Wnd>& b) -> std::shared_ptr<FD2D::SplitPanel>
    {
        auto sp = std::make_shared<FD2D::SplitPanel>(name, orientation);
        sp->SetSplitRatio(ratio);
        sp->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
        sp->SetFirstChild(a);
        sp->SetSecondChild(b);
        return sp;
    };

    // Equal-width row over panes[first..first+count): balanced binary tree
    // (2 -> 1+1, 3 -> 1+2, 4 -> 2+2) with top-level ratio leftCount/count so
    // every leaf is exactly 1/count of the row.
    std::function<std::shared_ptr<FD2D::Wnd>(size_t, size_t)> buildRow =
        [&](size_t first, size_t count) -> std::shared_ptr<FD2D::Wnd>
    {
        if (count == 1)
        {
            return panes[first];
        }
        const size_t leftCount = count / 2;
        auto left = buildRow(first, leftCount);
        auto right = buildRow(first + leftCount, count - leftCount);
        return makeSplit(
            L"hSplit_" + std::to_wstring(s_hostId++),
            FD2D::SplitterOrientation::Horizontal,
            static_cast<float>(leftCount) / static_cast<float>(count),
            left,
            right);
    };

    if (n <= 4)
    {
        return buildRow(0, n);
    }

    // Two-row grid: 5-6 -> 3 columns, 7-8 -> 4 columns. Top row fills first;
    // an odd count leaves the bottom row with fewer (wider) panes.
    const size_t topCount = (n <= 6) ? 3 : 4;
    auto top = buildRow(0, topCount);
    auto bottom = buildRow(topCount, n - topCount);
    return makeSplit(
        L"vSplit_" + std::to_wstring(s_hostId++),
        FD2D::SplitterOrientation::Vertical,
        0.5f,
        top,
        bottom);
}
