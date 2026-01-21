#pragma once

#include "FD2D/FD2D.h"

#include <wrl/client.h>

class ImageBrowserDragOverlay
{
public:
    enum class Kind
    {
        None,
        Replace,
        Insert
    };

    void Draw(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, Kind kind);

private:
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_replaceBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_insertBrush {};
};
