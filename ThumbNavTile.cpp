#include "ThumbNavTile.h"

#include "framework.h"
#include "Resource.h"
#include "ImageBrowserAssets.h"
#include "ThumbTileStyle.h"
#include "FD2D/Backplate.h"
#include "FD2D/Util.h"

#include <wincodec.h>
#include <vector>

ThumbNavTile::ThumbNavTile(const std::wstring& name)
    : Wnd(name)
    , m_label(name + L"_label")
    , m_badgeLabel(name + L"_badge")
{
    m_label.SetFont(L"Segoe UI Semibold", 18.0f);
    m_label.SetColor(D2D1::ColorF(D2D1::ColorF::White, 0.90f));
    m_label.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_label.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_label.SetEllipsisTrimmingEnabled(true);

    m_badgeLabel.SetFont(L"Segoe UI Semibold", 10.0f);
    m_badgeLabel.SetColor(D2D1::ColorF(D2D1::ColorF::White, 0.98f));
    m_badgeLabel.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_badgeLabel.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void ThumbNavTile::SetIcon(IconKind kind)
{
    if (m_icon == kind)
    {
        return;
    }

    m_icon = kind;
    m_folderBitmap.Reset();
    m_folderBitmapTarget = nullptr;
    m_folderBitmapKind = IconKind::None;
    Invalidate();
}

void ThumbNavTile::SetText(const std::wstring& text)
{
    m_label.SetText(text);
}

void ThumbNavTile::SetBadgeText(const std::wstring& text)
{
    m_badgeText = text;
    m_badgeLabel.SetText(text);
    Invalidate();
}

void ThumbNavTile::SetIconTint(IconTint tint)
{
    if (m_iconTint == tint)
    {
        return;
    }

    m_iconTint = tint;
    m_folderBitmap.Reset();
    m_folderBitmapTarget = nullptr;
    m_folderBitmapKind = IconKind::None;
    Invalidate();
}

void ThumbNavTile::SetTextPlacement(TextPlacement placement)
{
    m_textPlacement = placement;
    Invalidate();
}

void ThumbNavTile::SetFixedSize(const FD2D::Size& size)
{
    m_fixedSize = size;
}

void ThumbNavTile::SetSelected(bool selected)
{
    if (m_selected == selected)
    {
        return;
    }

    m_selected = selected;
    m_selectedStartMs = GetTickCount64();
    Invalidate();
    if (m_selected && BackplateRef() != nullptr)
    {
        BackplateRef()->RequestAnimationFrame();
    }
}

bool ThumbNavTile::Selected() const
{
    return m_selected;
}

void ThumbNavTile::SetOnClick(ClickHandler handler)
{
    m_onClick = std::move(handler);
}

void ThumbNavTile::SetOnDoubleClick(DoubleClickHandler handler)
{
    m_onDoubleClick = std::move(handler);
}

FD2D::Size ThumbNavTile::Measure(FD2D::Size available)
{
    UNREFERENCED_PARAMETER(available);
    m_desired = { m_fixedSize.w + 2.0f * m_margin, m_fixedSize.h + 2.0f * m_margin };
    return m_desired;
}

void ThumbNavTile::Arrange(FD2D::Rect finalRect)
{
    Wnd::Arrange(finalRect);
    const auto r = LayoutRect();
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;

    // Keep name layout behavior consistent with image thumbnail captions:
    // - scale font with tile height
    // - apply ellipsis within the tile width
    const float font = ThumbTileStyle::CaptionFontSize(h);
    m_label.SetFixedWidth((std::max)(0.0f, w));

    if (m_icon != IconKind::None && m_textPlacement == TextPlacement::Bottom)
    {
        // Reserve bottom area for the text when an icon is shown.
        const float labelH = (std::max)(28.0f, h * 0.34f);
        m_labelRect = D2D1::RectF(r.left, r.bottom - labelH, r.right, r.bottom);
        m_label.SetRect(m_labelRect);
        m_label.SetFont(ThumbTileStyle::kCaptionFontFamily, font);
        m_label.SetColor(D2D1::ColorF(D2D1::ColorF::White, 0.90f));
    }
    else
    {
        m_labelRect = r;
        m_label.SetRect(m_labelRect);
        // Centered text (folder/up): match image caption sizing and use black for readability on the icon.
        m_label.SetFont(ThumbTileStyle::kCaptionFontFamily, font);
        m_label.SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.90f));
    }

    if (!m_badgeText.empty())
    {
        const float badgeH = (std::max)(18.0f, h * 0.18f);
        const float badgeW = (std::max)(36.0f, w * 0.32f);
        const float inset = (std::max)(4.0f, h * 0.05f);
        m_badgeRect = D2D1::RectF(r.right - badgeW - inset, r.top + inset, r.right - inset, r.top + inset + badgeH);
        m_badgeLabel.SetRect(m_badgeRect);
        m_badgeLabel.SetFixedWidth(badgeW);
        m_badgeLabel.SetFont(L"Segoe UI Semibold", (std::max)(9.0f, (std::min)(12.0f, h * 0.10f)));
    }
    else
    {
        m_badgeRect = D2D1::RectF(0, 0, 0, 0);
    }
}

bool ThumbNavTile::OnInputEvent(const FD2D::InputEvent& event)
{
    switch (event.type)
    {
    case FD2D::InputEventType::MouseMove:
    {
        if (!event.hasPoint)
        {
            return false;
        }
        POINT pt = event.point;
        bool prevHover = m_hovered;
        m_hovered = HitTest(pt);
        if (m_hovered != prevHover)
        {
            Invalidate();
        }
        return m_hovered;
    }
    case FD2D::InputEventType::MouseDown:
    {
        if (event.button != FD2D::MouseButton::Left || !event.hasPoint)
        {
            break;
        }
        POINT pt = event.point;
        if (HitTest(pt))
        {
            m_pressed = true;
            if (m_onClick)
            {
                m_onClick();
            }
            Invalidate();
            return true;
        }
        break;
    }
    case FD2D::InputEventType::MouseUp:
    {
        if (event.button != FD2D::MouseButton::Left || !event.hasPoint)
        {
            break;
        }
        bool wasPressed = m_pressed;
        m_pressed = false;

        POINT pt = event.point;
        if (wasPressed)
        {
            Invalidate();
            return true;
        }
        break;
    }
    case FD2D::InputEventType::MouseDoubleClick:
    {
        if (event.button != FD2D::MouseButton::Left || !event.hasPoint)
        {
            break;
        }
        POINT pt = event.point;
        if (HitTest(pt))
        {
            if (m_onDoubleClick)
            {
                m_onDoubleClick();
            }
            return true;
        }
        break;
    }
    default:
        break;
    }

    return Wnd::OnInputEvent(event);
}

void ThumbNavTile::OnRender(ID2D1RenderTarget* target)
{
    if (target == nullptr)
    {
        return;
    }

    if (!m_strokeBrush)
    {
        target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_strokeBrush);
    }

    float strokeAlpha = 0.25f;
    float strokeThickness = 1.5f;
    D2D1_COLOR_F stroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, strokeAlpha);

    if (m_selected)
    {
        // Breathe animation: modulate alpha + thickness + slight expand.
        unsigned long long nowMs = GetTickCount64();
        float t = static_cast<float>((nowMs - m_selectedStartMs) % 1800) / 1800.0f;
        float s = 0.5f + 0.5f * sinf(t * 6.2831853f);
        float a = 0.52f + (0.20f * s);
        stroke = D2D1::ColorF(1.0f, 0.60f, 0.24f, a);

        const float inflate = 0.6f + (1.2f * s);
        D2D1_RECT_F pulseRect = LayoutRect();
        pulseRect.left -= inflate;
        pulseRect.top -= inflate;
        pulseRect.right += inflate;
        pulseRect.bottom += inflate;

        strokeThickness = 1.8f + (1.0f * s);

        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }

        m_strokeBrush->SetColor(stroke);
        target->DrawRectangle(pulseRect, m_strokeBrush.Get(), strokeThickness);
    }
    else
    {
        m_strokeBrush->SetColor(stroke);
        target->DrawRectangle(LayoutRect(), m_strokeBrush.Get(), strokeThickness);
    }

    // Caption backdrop (same idea as image thumbnail captions): a subtle translucent band.
    if (m_icon != IconKind::None && m_textPlacement == TextPlacement::Bottom)
    {
        if (!m_labelBackdropBrush)
        {
            (void)target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &m_labelBackdropBrush);
        }
        if (m_labelBackdropBrush)
        {
            target->FillRectangle(m_labelRect, m_labelBackdropBrush.Get());
        }
    }

    if (!m_badgeText.empty())
    {
        if (!m_badgeBackdropBrush)
        {
            (void)target->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.03f, 0.03f, 0.80f), &m_badgeBackdropBrush);
        }
        if (m_badgeBackdropBrush)
        {
            target->FillRectangle(m_badgeRect, m_badgeBackdropBrush.Get());
        }
        m_badgeLabel.OnRender(target);
    }

    // Draw icon (folder / up) if requested.
    if (m_icon != IconKind::None)
    {
        const auto r = LayoutRect();
        const float w = r.right - r.left;
        const float h = r.bottom - r.top;
        const float minSide = (std::min)(w, h);

        // Icon bounds (bitmap).
        const float iconW = minSide * 0.72f;
        const float iconH = iconW; // square icon
        const float iconX = r.left + (w - iconW) * 0.5f;
        const float iconY = r.top + (h * 0.40f) - (iconH * 0.5f);

        if (EnsureFolderBitmap(target))
        {
            const D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + iconW, iconY + iconH);
            // High-quality scaling for the icon.
            target->DrawBitmap(
                m_folderBitmap.Get(),
                dst,
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                D2D1::RectF(0.0f, 0.0f, m_folderBitmap->GetSize().width, m_folderBitmap->GetSize().height));
        }
    }

    m_label.OnRender(target);

    Wnd::OnRender(target);
}

bool ThumbNavTile::EnsureFolderBitmap(ID2D1RenderTarget* target)
{
    if (target == nullptr)
    {
        return false;
    }

    if (m_folderBitmap && m_folderBitmapTarget == target && m_folderBitmapKind == m_icon)
    {
        return true;
    }

    m_folderBitmap.Reset();
    m_folderBitmapTarget = nullptr;
    m_folderBitmapKind = IconKind::None;

    D2D1_COLOR_F tintColor {};
    const D2D1_COLOR_F* tint = nullptr;
    if (m_iconTint == IconTint::ArchiveRed)
    {
        tintColor = D2D1::ColorF(1.0f, 0.26f, 0.26f, 1.0f);
        tint = &tintColor;
    }
    else if (m_iconTint == IconTint::ArchiveBlue)
    {
        tintColor = D2D1::ColorF(0.26f, 0.55f, 1.0f, 1.0f);
        tint = &tintColor;
    }

    const auto kind = (m_icon == IconKind::Up)
        ? ImageBrowserAssets::FolderIconKind::FolderUp
        : ImageBrowserAssets::FolderIconKind::Folder;

    if (!ImageBrowserAssets::CreateFolderIconBitmap(target, kind, tint, m_folderBitmap))
    {
        return false;
    }

    m_folderBitmapTarget = target;
    m_folderBitmapKind = m_icon;
    return true;
}

bool ThumbNavTile::HitTest(const POINT& pt) const
{
    return FD2D::Util::RectContainsPoint(LayoutRect(), pt);
}

