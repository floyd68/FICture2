#include "TestHarness.h"

#include "ImageBrowserSplitCoordinator.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

// The root browser's side-by-side panes: order, the viewer limit, closing by
// name, the equal-width host tree and its split ratios. No windows: the
// panes are plain FD2D windows.

namespace
{
    using Panes = ImageBrowserSplitPanes::Panes;

    std::shared_ptr<FD2D::Wnd> Pane(const std::wstring& name)
    {
        return std::make_shared<FD2D::Wnd>(name);
    }

    std::vector<std::wstring> Names(const Panes& panes)
    {
        std::vector<std::wstring> names;
        for (const auto& pane : panes)
        {
            names.push_back(pane->Name());
        }
        return names;
    }

    ImageBrowserSplitPanes WithPanes(size_t count)
    {
        ImageBrowserSplitPanes panes;
        panes.EnsureRoot(Pane(L"root"));
        for (size_t i = 1; i < count; ++i)
        {
            panes.Append(Pane(L"pane" + std::to_wstring(i)));
        }
        return panes;
    }

    bool IsPane(const Panes& panes, const std::shared_ptr<FD2D::Wnd>& node)
    {
        for (const auto& pane : panes)
        {
            if (pane == node)
            {
                return true;
            }
        }
        return false;
    }

    // The panes in the host tree, left to right and top to bottom.
    void Leaves(const std::shared_ptr<FD2D::Wnd>& node, const Panes& panes, Panes& out)
    {
        if (IsPane(panes, node))
        {
            out.push_back(node);
            return;
        }
        for (const auto& child : node->ChildrenInOrder())
        {
            Leaves(child, panes, out);
        }
    }

    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }

    bool NearAll(const std::vector<float>& actual, const std::vector<float>& expected)
    {
        if (actual.size() != expected.size())
        {
            return false;
        }
        for (size_t i = 0; i < actual.size(); ++i)
        {
            if (!Near(actual[i], expected[i]))
            {
                return false;
            }
        }
        return true;
    }
}

FIC2_TEST(ImageBrowserSplitPanes_RootComesFirstAndOnce)
{
    ImageBrowserSplitPanes panes;
    panes.EnsureRoot(nullptr);
    FIC2_CHECK(panes.Empty());

    const auto root = Pane(L"root");
    panes.EnsureRoot(root);
    panes.EnsureRoot(Pane(L"other"));
    FIC2_REQUIRE(panes.Count() == 1);
    FIC2_CHECK(panes.All()[0] == root);
}

FIC2_TEST(ImageBrowserSplitPanes_InsertAfterANamedPane)
{
    ImageBrowserSplitPanes panes = WithPanes(1);
    panes.Append(Pane(L"a"));
    panes.Append(Pane(L"b"));
    panes.InsertAfter(L"a", Pane(L"c"));
    panes.InsertAfter(L"root", Pane(L"d"));
    panes.InsertAfter(L"missing", Pane(L"e"));
    FIC2_CHECK((Names(panes.All()) == std::vector<std::wstring> { L"root", L"d", L"a", L"c", L"b", L"e" }));
}

FIC2_TEST(ImageBrowserSplitPanes_CloseRemovesOnlyAddedPanes)
{
    ImageBrowserSplitPanes panes = WithPanes(3);   // root, pane1, pane2
    FIC2_CHECK(!panes.RemoveAdded(L"root"));
    FIC2_CHECK(!panes.RemoveAdded(L"missing"));
    FIC2_CHECK(panes.RemoveAdded(L"pane1"));
    FIC2_CHECK((Names(panes.All()) == std::vector<std::wstring> { L"root", L"pane2" }));
    FIC2_CHECK(!panes.RemoveAdded(L"pane1"));
}

FIC2_TEST(ImageBrowserSplitPanes_ViewerLimit)
{
    ImageBrowserSplitPanes panes = WithPanes(ImageBrowserSplitCoordinator::kMaxViewers - 1);
    FIC2_CHECK(panes.CanAdd());
    panes.Append(Pane(L"last"));
    FIC2_CHECK(panes.Count() == ImageBrowserSplitCoordinator::kMaxViewers);
    FIC2_CHECK(!panes.CanAdd());
    FIC2_CHECK(panes.RemoveAdded(L"last"));
    FIC2_CHECK(panes.CanAdd());
}

FIC2_TEST(ImageBrowserSplitPanes_HostTreeShapes)
{
    // One pane: the pane itself.
    ImageBrowserSplitPanes single = WithPanes(1);
    FIC2_CHECK(single.RebuildHost() == single.All()[0]);
    FIC2_CHECK(single.CaptureRatios().empty());

    // 1-4 panes: one row, every pane 1/n wide (3 -> 1 + 2).
    ImageBrowserSplitPanes row = WithPanes(3);
    const auto rowHost = std::dynamic_pointer_cast<FD2D::SplitPanel>(row.RebuildHost());
    FIC2_REQUIRE(rowHost != nullptr);
    FIC2_CHECK(rowHost->Orientation() == FD2D::SplitterOrientation::Horizontal);
    FIC2_CHECK(NearAll(row.CaptureRatios(), { 1.0f / 3.0f, 0.5f }));

    // 5-6 panes: two rows of up to 3; 7-8 panes: two rows of up to 4.
    struct Grid
    {
        size_t panes;
        std::vector<float> ratios;
    };
    const std::vector<Grid> grids {
        { 5, { 0.5f, 1.0f / 3.0f, 0.5f, 0.5f } },
        { 6, { 0.5f, 1.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.5f } },
        { 7, { 0.5f, 0.5f, 0.5f, 0.5f, 1.0f / 3.0f, 0.5f } },
        { 8, { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f } },
    };
    for (const Grid& grid : grids)
    {
        ImageBrowserSplitPanes panes = WithPanes(grid.panes);
        const auto host = std::dynamic_pointer_cast<FD2D::SplitPanel>(panes.RebuildHost());
        FIC2_REQUIRE(host != nullptr);
        FIC2_CHECK(host->Orientation() == FD2D::SplitterOrientation::Vertical);
        FIC2_CHECK(NearAll(panes.CaptureRatios(), grid.ratios));
        Panes leaves;
        Leaves(host, panes.All(), leaves);
        FIC2_CHECK(leaves == panes.All());
    }
}

FIC2_TEST(ImageBrowserSplitPanes_RatiosStopAtThePanes)
{
    // Each pane is a split of its own (a browser's main/thumbnail split):
    // its ratio is neither captured nor changed.
    ImageBrowserSplitPanes panes;
    std::vector<std::shared_ptr<FD2D::SplitPanel>> inner;
    for (int i = 0; i < 3; ++i)
    {
        auto pane = std::make_shared<FD2D::SplitPanel>(L"pane" + std::to_wstring(i), FD2D::SplitterOrientation::Vertical);
        pane->SetFirstChild(Pane(L"main" + std::to_wstring(i)));
        pane->SetSecondChild(Pane(L"thumbs" + std::to_wstring(i)));
        pane->SetSplitRatio(0.7f);
        inner.push_back(pane);
        if (i == 0)
        {
            panes.EnsureRoot(pane);
        }
        else
        {
            panes.Append(pane);
        }
    }
    FIC2_REQUIRE(panes.RebuildHost() != nullptr);
    FIC2_CHECK(NearAll(panes.CaptureRatios(), { 1.0f / 3.0f, 0.5f }));

    panes.ApplyRatios({ 0.25f, 0.6f });
    FIC2_CHECK(NearAll(panes.CaptureRatios(), { 0.25f, 0.6f }));
    // Fewer ratios than splits: the rest keep theirs.
    panes.ApplyRatios({ 0.4f });
    FIC2_CHECK(NearAll(panes.CaptureRatios(), { 0.4f, 0.6f }));
    for (const auto& pane : inner)
    {
        FIC2_CHECK(Near(pane->SplitRatio(), 0.7f));
    }
}

FIC2_TEST(ImageBrowserSplitPanes_RebuildWithoutPanesKeepsTheHost)
{
    ImageBrowserSplitPanes panes;
    FIC2_CHECK(panes.RebuildHost() == nullptr);
    FIC2_CHECK(panes.Host() == nullptr);
    FIC2_CHECK(panes.CaptureRatios().empty());

    ImageBrowserSplitPanes two = WithPanes(2);
    const auto host = two.RebuildHost();
    FIC2_REQUIRE(host != nullptr);
    FIC2_CHECK(two.Host() == host);
}
