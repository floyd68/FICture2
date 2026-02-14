#include "ThumbImageTile.h"
#include "FD2D/Backplate.h"

#include "framework.h"
#include <algorithm>
#include <cmath>

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
    // Only invalidate if size actually changed (prevents unnecessary re-renders)
    if (std::abs(m_fixedSize.w - size.w) < 0.1f && std::abs(m_fixedSize.h - size.h) < 0.1f)
    {
        return;
    }

    m_useVariableWidth = false;
    m_fixedSize = size;
    if (m_image)
    {
        m_image->SetThumbnailSize({ m_fixedSize.w, m_fixedSize.h });
    }
    Invalidate();
}

void ThumbImageTile::SetFixedHeight(float height)
{
    if (std::abs(m_fixedHeight - height) < 0.1f && m_useVariableWidth)
    {
        return;
    }

    const bool wasVariableWidth = m_useVariableWidth;
    m_useVariableWidth = true;
    m_fixedHeight = height;
    
    // Reset cached bitmap size when entering variable-width mode so first decode can trigger relayout.
    if (!wasVariableWidth)
    {
        m_lastBitmapSize = D2D1::SizeF(0.0f, 0.0f);
    }

    if (m_image)
    {
        // Keep decode target square to avoid extreme target-width hints.
        // Aspect ratio is preserved in decode pipeline and render path.
        m_image->SetThumbnailSize({ height, height });
    }
    
    Invalidate();
    
    if (BackplateRef() != nullptr)
    {
        BackplateRef()->RequestLayout();
    }
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
    
    if (m_useVariableWidth && m_image)
    {
        // Get actual bitmap size to calculate aspect ratio
        const D2D1_SIZE_F bitmapSize = m_image->GetBitmapSize();
        
        float width = m_fixedHeight;
        if (bitmapSize.width > 0.0f && bitmapSize.height > 0.0f)
        {
            const float aspectRatio = bitmapSize.width / bitmapSize.height;
            width = m_fixedHeight * aspectRatio;
        }
        else
        {
            // Before image loads, use square as placeholder
            width = m_fixedHeight;
        }
        
        m_desired = { width + 2.0f * m_margin, m_fixedHeight + 2.0f * m_margin };
    }
    else
    {
        m_desired = { m_fixedSize.w + 2.0f * m_margin, m_fixedSize.h + 2.0f * m_margin };
    }
    
    return m_desired;
}

void ThumbImageTile::Arrange(FD2D::Rect finalRect)
{
    Wnd::Arrange(finalRect);
    const auto r = LayoutRect();
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;

    if (m_useVariableWidth)
    {
        // Variable width mode: use full area respecting aspect ratio
        if (m_image)
        {
            const FD2D::Rect imageRect { r.left, r.top, w, h };
            m_image->Arrange(imageRect);
        }

        const float labelH = (std::max)(18.0f, h * 0.28f);
        m_labelRect = D2D1::RectF(r.left, r.bottom - labelH, r.right, r.bottom);

        const float font = (std::max)(10.0f, (std::min)(14.0f, h * 0.11f));
        m_label.SetFont(L"Segoe UI", font);
        m_label.SetRect(m_labelRect);
        m_label.SetFixedWidth(w);
    }
    else
    {
        // Fixed size mode: calculate square area for the thumbnail image (centered in tile)
        const float squareSize = (std::min)(w, h);
        
        // Center the square image in the tile
        const float xOffset = (w - squareSize) * 0.5f;
        const float yOffset = (h - squareSize) * 0.5f;
        
        const FD2D::Rect imageRect
        {
            r.left + xOffset,
            r.top + yOffset,
            squareSize,
            squareSize
        };

        if (m_image)
        {
            m_image->Arrange(imageRect);
        }

        const float labelH = (std::max)(18.0f, h * 0.28f);
        m_labelRect = D2D1::RectF(r.left, r.bottom - labelH, r.right, r.bottom);

        const float font = (std::max)(10.0f, (std::min)(14.0f, h * 0.11f));
        m_label.SetFont(L"Segoe UI", font);
        m_label.SetRect(m_labelRect);
        m_label.SetFixedWidth(w);
    }
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

    // Skip rendering when the tile is completely outside the current backplate viewport.
    // This reduces startup/render spikes when many offscreen thumbnails exist.
    FD2D::Backplate* bp = BackplateRef();
    if (bp != nullptr)
    {
        const D2D1_SIZE_U client = bp->ClientSize();
        if (client.width > 0 && client.height > 0)
        {
            const D2D1_RECT_F localRect = LayoutRect();
            D2D1_MATRIX_3X2_F transform {};
            target->GetTransform(&transform);

            const auto tx = [&](float x, float y) -> D2D1_POINT_2F
            {
                return D2D1::Point2F(
                    transform._11 * x + transform._21 * y + transform._31,
                    transform._12 * x + transform._22 * y + transform._32);
            };

            const D2D1_POINT_2F p0 = tx(localRect.left, localRect.top);
            const D2D1_POINT_2F p1 = tx(localRect.right, localRect.top);
            const D2D1_POINT_2F p2 = tx(localRect.left, localRect.bottom);
            const D2D1_POINT_2F p3 = tx(localRect.right, localRect.bottom);

            const float minX = (std::min)((std::min)(p0.x, p1.x), (std::min)(p2.x, p3.x));
            const float maxX = (std::max)((std::max)(p0.x, p1.x), (std::max)(p2.x, p3.x));
            const float minY = (std::min)((std::min)(p0.y, p1.y), (std::min)(p2.y, p3.y));
            const float maxY = (std::max)((std::max)(p0.y, p1.y), (std::max)(p2.y, p3.y));

            const float viewportL = 0.0f;
            const float viewportT = 0.0f;
            const float viewportR = static_cast<float>(client.width);
            const float viewportB = static_cast<float>(client.height);

            const bool intersects =
                (maxX >= viewportL) &&
                (maxY >= viewportT) &&
                (minX <= viewportR) &&
                (minY <= viewportB);

            if (!intersects)
            {
                return;
            }
        }
    }

    // Check if bitmap size changed (in variable width mode) and request layout if needed
    if (m_useVariableWidth && m_image)
    {
        const D2D1_SIZE_F currentSize = m_image->GetBitmapSize();
        const bool sizeChanged = (currentSize.width != m_lastBitmapSize.width || currentSize.height != m_lastBitmapSize.height);
        const bool isLoaded = (currentSize.width > 0.0f && currentSize.height > 0.0f);
        
        if (sizeChanged && isLoaded)
        {
            m_lastBitmapSize = currentSize;

            // Force parent container to re-layout
            // Note: RequestLayout during rendering will set m_renderRequested flag
            // and the layout will be updated in the next render loop iteration
            if (bp != nullptr)
            {
                if (!bp->IsInSizeMove())
                {
                    bp->RequestLayout();
                }
            }
        }
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

