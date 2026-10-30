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

// The root browser's side-by-side panes and the host tree that lays them out.
// Pane 0 is the root browser's own UI root (its main/thumbnail split); the
// others are the browsers added beside it. The host tree is rebuilt on demand
// (RebuildHost) and is what the root browser shows as its child.
class ImageBrowserSplitPanes
{
public:
    using Panes = std::vector<std::shared_ptr<FD2D::Wnd>>;

    const Panes& All() const { return m_panes; }
    size_t Count() const { return m_panes.size(); }
    bool Empty() const { return m_panes.empty(); }
    bool CanAdd() const { return ImageBrowserSplitCoordinator::CanAddViewer(m_panes.size()); }

    // Makes `root` pane 0 when there are no panes yet (a null root adds none).
    void EnsureRoot(const std::shared_ptr<FD2D::Wnd>& root);
    void Append(const std::shared_ptr<FD2D::Wnd>& pane);
    // Inserts `pane` after the pane named `afterName`, or appends it when no
    // pane has that name.
    void InsertAfter(const std::wstring& afterName, const std::shared_ptr<FD2D::Wnd>& pane);
    // Removes the added pane named `name` (never pane 0). Returns true if one
    // was removed.
    bool RemoveAdded(const std::wstring& name);

    // Rebuilds the equal-width host tree over the panes and returns it; null
    // (and the previous host kept) when there are no panes.
    std::shared_ptr<FD2D::Wnd> RebuildHost();
    const std::shared_ptr<FD2D::Wnd>& Host() const { return m_host; }

    // The host tree's split ratios in preorder, including the two-row
    // vertical split. Stops at the panes, so a browser's own main/thumbnail
    // split is not included.
    std::vector<float> CaptureRatios() const;
    // Applies ratios captured by CaptureRatios, in the same order; splits
    // beyond the given ratios keep theirs.
    void ApplyRatios(const std::vector<float>& ratios);

private:
    bool IsPane(const std::shared_ptr<FD2D::Wnd>& node) const;
    void CaptureRatios(const std::shared_ptr<FD2D::Wnd>& node, std::vector<float>& out) const;
    void ApplyRatios(const std::shared_ptr<FD2D::Wnd>& node, const std::vector<float>& ratios, size_t& index);

    Panes m_panes {};
    std::shared_ptr<FD2D::Wnd> m_host {};
};
