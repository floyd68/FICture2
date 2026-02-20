#include "ImageBrowserSplitCoordinator.h"

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
    if (n == 1)
    {
        return panes[0];
    }

    auto makeSplit = [](
        const std::wstring& name,
        float ratio,
        const std::shared_ptr<FD2D::Wnd>& a,
        const std::shared_ptr<FD2D::Wnd>& b) -> std::shared_ptr<FD2D::SplitPanel>
    {
        auto sp = std::make_shared<FD2D::SplitPanel>(name, FD2D::SplitterOrientation::Horizontal);
        sp->SetSplitRatio(ratio);
        sp->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
        sp->SetFirstChild(a);
        sp->SetSecondChild(b);
        return sp;
    };

    static int s_hostId = 1;

    if (n == 2)
    {
        return makeSplit(L"hSplit2_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
    }
    if (n == 3)
    {
        // 1/3 | (2/3 split into 1/2 + 1/2) => 1/3, 1/3, 1/3
        auto right = makeSplit(L"hSplit3_r_" + std::to_wstring(s_hostId++), 0.5f, panes[1], panes[2]);
        return makeSplit(L"hSplit3_" + std::to_wstring(s_hostId++), 1.0f / 3.0f, panes[0], right);
    }

    // n == 4
    auto left = makeSplit(L"hSplit4_l_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
    auto right = makeSplit(L"hSplit4_r_" + std::to_wstring(s_hostId++), 0.5f, panes[2], panes[3]);
    return makeSplit(L"hSplit4_" + std::to_wstring(s_hostId++), 0.5f, left, right);
}
