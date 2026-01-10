#include "ThumbImageTile.h"

#include "framework.h"

ThumbImageTile::ThumbImageTile(const std::wstring& name)
    : Wnd(name)
    , m_label(name + L"_label")
{
    m_image = std::make_shared<FD2D::ThumbImage>(name + L"_img");
    m_image->SetThumbnailSize({ m_fixedSize.w, m_fixedSize.h });
    AddChild(m_image);

    m_label.SetFont(L"Segoe UI", 12.0f);
    m_label.SetColor(D2D1::ColorF(D2D1::ColorF::White, 0.95f));
    m_label.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_label.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_label.SetEllipsisTrimmingEnabled(true);

    m_image->SetOnClick([this]()
    {
        if (m_onClick)
        {
            m_onClick();
        }
    });
}

void ThumbImageTile::SetFixedSize(const FD2D::Size& size)
{
    m_fixedSize = size;
    if (m_image)
    {
        m_image->SetThumbnailSize({ m_fixedSize.w, m_fixedSize.h });
    }
    Invalidate();
}

void ThumbImageTile::SetCaption(const std::wstring& text)
{
    m_label.SetText(text);
    Invalidate();
}

void ThumbImageTile::SetSourceFile(const std::wstring& path)
{
    if (m_image)
    {
        m_image->SetSourceFile(path);
    }
}

void ThumbImageTile::SetSelected(bool selected)
{
    if (m_image)
    {
        m_image->SetSelected(selected);
    }
    Invalidate();
}

void ThumbImageTile::SetOnClick(ClickHandler handler)
{
    m_onClick = std::move(handler);
}

FD2D::Size ThumbImageTile::Measure(FD2D::Size available)
{
    UNREFERENCED_PARAMETER(available);
    m_desired = { m_fixedSize.w + 2.0f * m_margin, m_fixedSize.h + 2.0f * m_margin };
    return m_desired;
}

void ThumbImageTile::Arrange(FD2D::Rect finalRect)
{
    Wnd::Arrange(finalRect);
    const auto r = LayoutRect();

    if (m_image)
    {
        m_image->Arrange(finalRect);
    }

    const float h = r.bottom - r.top;
    const float labelH = (std::max)(18.0f, h * 0.28f);
    m_labelRect = D2D1::RectF(r.left, r.bottom - labelH, r.right, r.bottom);

    const float font = (std::max)(10.0f, (std::min)(14.0f, h * 0.11f));
    m_label.SetFont(L"Segoe UI", font);
    m_label.SetRect(m_labelRect);
    m_label.SetFixedWidth(r.right - r.left);
}

void ThumbImageTile::EnsureResources(ID2D1RenderTarget* target)
{
    if (!target)
    {
        return;
    }
    if (!m_labelBackdropBrush)
    {
        (void)target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &m_labelBackdropBrush);
    }
}

void ThumbImageTile::OnRender(ID2D1RenderTarget* target)
{
    if (!target)
    {
        return;
    }

    // Render the image child first (thumbnail + selection overlay).
    if (m_image)
    {
        m_image->OnRender(target);
    }

    EnsureResources(target);

    // Caption overlay (bottom band).
    if (m_labelBackdropBrush)
    {
        target->FillRectangle(m_labelRect, m_labelBackdropBrush.Get());
    }

    m_label.OnRender(target);
}

