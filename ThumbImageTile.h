#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Text.h"
#include "FD2D/ThumbImage.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>

// Thumbnail tile for images:
// - Renders an FD2D::Image thumbnail
// - Draws the filename inside the thumbnail at the bottom
class ThumbImageTile : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;

    explicit ThumbImageTile(const std::wstring& name);

    void SetFixedSize(const FD2D::Size& size);
    void SetFixedHeight(float height);
    void SetCaption(const std::wstring& text);

    void SetSourceFile(const std::wstring& path);
    void SetSelected(bool selected);
    void SetOnClick(ClickHandler handler);

    std::shared_ptr<FD2D::ThumbImage> ImageWnd() const { return m_image; }

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;
    void OnRender(ID2D1RenderTarget* target) override;

private:
    void EnsureResources(ID2D1RenderTarget* target);

    FD2D::Size m_fixedSize { 128.0f, 128.0f };
    float m_fixedHeight { 128.0f };
    bool m_useVariableWidth { false };
    D2D1_SIZE_F m_lastBitmapSize {};
    std::shared_ptr<FD2D::ThumbImage> m_image {};
    FD2D::Text m_label;
    ClickHandler m_onClick {};

    D2D1_RECT_F m_labelRect {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_labelBackdropBrush {};
};

