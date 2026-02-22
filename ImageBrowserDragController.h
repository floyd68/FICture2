#pragma once

#include "FD2D/FD2D.h"
#include "VirtualPath.h"

#include <wrl/client.h>
#include <string>

class ImageBrowserDragController
{
public:
    enum class OverlayKind
    {
        None,
        Replace,
        Insert
    };

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
        OverlayKind& outOverlay) const;

    void DrawOverlay(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, OverlayKind kind);
    void SetInsertThreshold(float threshold);
    float InsertThreshold() const { return m_insertThreshold; }

private:
    float m_insertThreshold { 0.75f };
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_replaceBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_insertBrush {};
};
