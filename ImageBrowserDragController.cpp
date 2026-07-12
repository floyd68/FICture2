#include "ImageBrowserDragController.h"
#include "FD2D/Util.h"

#include "ImageAwareVfs.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"
#include <algorithm>


bool ImageBrowserDragController::HandleFileDrop(
    const std::wstring& path,
    const POINT& clientPt,
    const D2D1_RECT_F& mainRect,
    Action& outAction) const
{
    if (path.empty())
    {
        return false;
    }

    if (!FD2D::Util::RectContainsPoint(mainRect, clientPt))
    {
        return false;
    }

    auto vp = Floar::VirtualPath::Parse(path);
    if (!vp)
    {
        return false;
    }

    if (!ImageAwareVfs::IsBrowsableDropTarget(*vp))
    {
        return false;
    }

    const float mainW = (std::max)(1.0f, mainRect.right - mainRect.left);
    const float relX = (static_cast<float>(clientPt.x) - mainRect.left) / mainW;
    if (relX >= m_insertThreshold)
    {
        outAction.kind = ActionKind::InsertHorizontal;
        outAction.path = *vp;
        return true;
    }

    if (Floar::VirtualFileSystem::IsDirectory(*vp) || vp->IsArchiveFile())
    {
        outAction.kind = ActionKind::NavigateToFolder;
        outAction.path = *vp;
        return true;
    }

    outAction.kind = ActionKind::NavigateToFile;
    outAction.path = *vp;
    return true;
}

bool ImageBrowserDragController::HandleFileDrag(
    const std::wstring& path,
    const POINT& clientPt,
    const D2D1_RECT_F& mainRect,
    FD2D::FileDragVisual& outVisual,
    OverlayKind& outOverlay) const
{
    if (path.empty())
    {
        return false;
    }

    if (!FD2D::Util::RectContainsPoint(mainRect, clientPt))
    {
        return false;
    }

    auto vp = Floar::VirtualPath::Parse(path);
    if (!vp)
    {
        return false;
    }

    if (!ImageAwareVfs::IsBrowsableDropTarget(*vp))
    {
        return false;
    }

    const float w = (std::max)(1.0f, mainRect.right - mainRect.left);
    const float relX = (static_cast<float>(clientPt.x) - mainRect.left) / w;
    if (relX < m_insertThreshold)
    {
        outOverlay = OverlayKind::Replace;
        outVisual = FD2D::FileDragVisual::Replace;
        return true;
    }

    outOverlay = OverlayKind::Insert;
    outVisual = FD2D::FileDragVisual::Insert;
    return true;
}

void ImageBrowserDragController::DrawOverlay(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, OverlayKind kind)
{
    if (!target || kind == OverlayKind::None)
    {
        return;
    }

    if (!m_replaceBrush)
    {
        (void)target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.18f), m_replaceBrush.ReleaseAndGetAddressOf());
    }
    if (!m_insertBrush)
    {
        (void)target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 0.18f), m_insertBrush.ReleaseAndGetAddressOf());
    }

    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    if (kind == OverlayKind::Replace && m_replaceBrush)
    {
        target->FillRectangle(rect, m_replaceBrush.Get());
    }
    else if (m_insertBrush)
    {
        const float w = (std::max)(1.0f, rect.right - rect.left);
        const float splitX = rect.left + (w * m_insertThreshold);
        const D2D1_RECT_F rr { splitX, rect.top, rect.right, rect.bottom };
        target->FillRectangle(rr, m_insertBrush.Get());
    }
}

void ImageBrowserDragController::SetInsertThreshold(float threshold)
{
    // Keep usable split area on both sides.
    m_insertThreshold = (std::max)(0.10f, (std::min)(0.95f, threshold));
}
