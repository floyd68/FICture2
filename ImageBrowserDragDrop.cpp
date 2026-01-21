#include "ImageBrowserDragDrop.h"

#include "VirtualFileSystem.h"

namespace
{
    bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
    {
        return pt.x >= r.left &&
            pt.x <= r.right &&
            pt.y >= r.top &&
            pt.y <= r.bottom;
    }

    bool IsSupportedDropPath(const VirtualPath& path)
    {
        if (VirtualFileSystem::IsDirectory(path) || path.IsArchiveFile())
        {
            return true;
        }

        return VirtualFileSystem::IsImageFile(path);
    }
}

bool ImageBrowserDragDrop::HandleFileDrop(
    const std::wstring& path,
    const POINT& clientPt,
    const D2D1_RECT_F& mainRect,
    Action& outAction) const
{
    if (path.empty())
    {
        return false;
    }

    if (!RectContainsPoint(mainRect, clientPt))
    {
        return false;
    }

    auto vp = VirtualPath::Parse(path);
    if (!vp)
    {
        return false;
    }

    if (!IsSupportedDropPath(*vp))
    {
        return false;
    }

    const float mainW = (std::max)(1.0f, mainRect.right - mainRect.left);
    const float relX = (static_cast<float>(clientPt.x) - mainRect.left) / mainW;
    if (relX >= 0.75f)
    {
        outAction.kind = ActionKind::InsertHorizontal;
        outAction.path = *vp;
        return true;
    }

    if (VirtualFileSystem::IsDirectory(*vp) || vp->IsArchiveFile())
    {
        outAction.kind = ActionKind::NavigateToFolder;
        outAction.path = *vp;
        return true;
    }

    outAction.kind = ActionKind::NavigateToFile;
    outAction.path = *vp;
    return true;
}

bool ImageBrowserDragDrop::HandleFileDrag(
    const std::wstring& path,
    const POINT& clientPt,
    const D2D1_RECT_F& mainRect,
    FD2D::FileDragVisual& outVisual,
    ImageBrowserDragOverlay::Kind& outOverlay) const
{
    if (path.empty())
    {
        return false;
    }

    if (!RectContainsPoint(mainRect, clientPt))
    {
        return false;
    }

    auto vp = VirtualPath::Parse(path);
    if (!vp)
    {
        return false;
    }

    if (!IsSupportedDropPath(*vp))
    {
        return false;
    }

    const float w = (std::max)(1.0f, mainRect.right - mainRect.left);
    const float relX = (static_cast<float>(clientPt.x) - mainRect.left) / w;
    if (relX < 0.75f)
    {
        outOverlay = ImageBrowserDragOverlay::Kind::Replace;
        outVisual = FD2D::FileDragVisual::Replace;
        return true;
    }

    outOverlay = ImageBrowserDragOverlay::Kind::Insert;
    outVisual = FD2D::FileDragVisual::Insert;
    return true;
}
