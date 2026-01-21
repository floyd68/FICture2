#pragma once

#include "FD2D/FD2D.h"
#include "ImageBrowserDragOverlay.h"
#include "VirtualPath.h"

#include <memory>
#include <string>

class ImageBrowserDragDrop
{
public:
    enum class ActionKind
    {
        None,
        NavigateToFolder,
        NavigateToFile,
        InsertHorizontal
    };

    struct Action
    {
        ActionKind kind { ActionKind::None };
        VirtualPath path {};
    };

    bool HandleFileDrop(
        const std::wstring& path,
        const POINT& clientPt,
        const D2D1_RECT_F& mainRect,
        Action& outAction) const;

    bool HandleFileDrag(
        const std::wstring& path,
        const POINT& clientPt,
        const D2D1_RECT_F& mainRect,
        FD2D::FileDragVisual& outVisual,
        ImageBrowserDragOverlay::Kind& outOverlay) const;
};
