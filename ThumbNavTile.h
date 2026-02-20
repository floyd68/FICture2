#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Text.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <functional>
#include <string>

class ThumbNavTile : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;
    using DoubleClickHandler = std::function<void()>;

    enum class IconKind
    {
        None,
        Folder,
        Up,
    };

    enum class TextPlacement
    {
        Center,
        Bottom,
    };

    enum class IconTint
    {
        None,
        ArchiveRed,
        ArchiveBlue,
    };

    explicit ThumbNavTile(const std::wstring& name);

    void SetIcon(IconKind kind);
    void SetText(const std::wstring& text);
    void SetBadgeText(const std::wstring& text);
    void SetIconTint(IconTint tint);
    void SetTextPlacement(TextPlacement placement);
    void SetFixedSize(const FD2D::Size& size);
    void SetSelected(bool selected);
    bool Selected() const;
    void SetOnClick(ClickHandler handler);
    void SetOnDoubleClick(DoubleClickHandler handler);

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;
    void OnRender(ID2D1RenderTarget* target) override;

private:
    bool EnsureFolderBitmap(ID2D1RenderTarget* target);
    bool HitTest(const POINT& pt) const;

    FD2D::Size m_fixedSize { 128.0f, 128.0f };
    FD2D::Text m_label;
    FD2D::Text m_badgeLabel;
    D2D1_RECT_F m_labelRect {};
    D2D1_RECT_F m_badgeRect {};
    ClickHandler m_onClick {};
    DoubleClickHandler m_onDoubleClick {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_labelBackdropBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeBackdropBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_fillBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_strokeBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_iconStrokeBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_iconAccentBrush {};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
    ID2D1RenderTarget* m_folderBitmapTarget { nullptr };
    IconKind m_icon { IconKind::None };
    IconTint m_iconTint { IconTint::None };
    TextPlacement m_textPlacement { TextPlacement::Bottom };
    bool m_hovered { false };
    bool m_pressed { false };
    bool m_selected { false };
    std::wstring m_badgeText {};
    unsigned long long m_selectedStartMs { 0 };
};

