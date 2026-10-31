#include "ImageBrowserMainImageView.h"

#include <algorithm>
#include <cmath>

namespace
{
    void RotateVectorByQuarters(float dx, float dy, int quarters, float& outX, float& outY)
    {
        switch (((quarters % 4) + 4) % 4)
        {
        case 1: outX = -dy; outY = dx;  break;
        case 2: outX = -dx; outY = -dy; break;
        case 3: outX = dy;  outY = -dx; break;
        default: outX = dx; outY = dy;  break;
        }
    }
}

void MainImageView::Set(float zoom, float targetZoom, float zoomVelocity, float panX, float panY,
    int rotationQuarters, unsigned long long nowMs)
{
    m_zoom = zoom;
    m_targetZoom = targetZoom;
    m_zoomVelocity = zoomVelocity;
    m_panX = panX;
    m_panY = panY;
    m_rotationQuarters = ((rotationQuarters % 4) + 4) % 4;
    m_dragging = false;
    m_pointerDown = false;
    m_wheelAnchorActive = false;
    m_lastStepMs = nowMs;
}

void MainImageView::SetTargetZoom(float zoom, unsigned long long nowMs)
{
    m_targetZoom = (std::max)(kMinZoom, (std::min)(kMaxZoom, zoom));
    m_lastStepMs = nowMs;
}

void MainImageView::ResetZoom(unsigned long long nowMs)
{
    m_targetZoom = 1.0f;
    m_zoomVelocity = 0.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
    m_dragging = false;
    m_wheelAnchorActive = false;
    m_lastStepMs = nowMs;
}

void MainImageView::SetStiffness(float stiffness)
{
    m_stiffness = (std::max)(10.0f, (std::min)(500.0f, stiffness));
}

bool MainImageView::ResetForNewSource(unsigned long long nowMs)
{
    const bool wasDragging = m_dragging;
    m_dragging = false;
    m_rotationQuarters = 0;
    m_wheelAnchorActive = false;
    m_zoomVelocity = 0.0f;
    m_lastStepMs = nowMs;
    return wasDragging;
}

void MainImageView::EndGestures()
{
    m_pointerDown = false;
    m_dragging = false;
    m_wheelAnchorActive = false;
}

MainImageView::Step MainImageView::Advance(unsigned long long nowMs, float centerX, float centerY,
    const std::optional<PanBounds>& bounds)
{
    if (m_lastStepMs == 0)
    {
        m_lastStepMs = nowMs;
    }
    const unsigned long long elapsed = nowMs - m_lastStepMs;
    m_lastStepMs = nowMs;
    if (elapsed == 0)
    {
        return Step::Idle;
    }

    // Critically damped spring toward the target.
    const float dt = static_cast<float>(elapsed) / 1000.0f;
    const float damping = 2.0f * std::sqrt(m_stiffness);
    const float diff = m_targetZoom - m_zoom;
    m_zoomVelocity += (diff * m_stiffness - m_zoomVelocity * damping) * dt;
    m_zoom += m_zoomVelocity * dt;

    if (m_wheelAnchorActive && !m_dragging)
    {
        const float startZoom = (m_wheelStartZoom > 0.0001f) ? m_wheelStartZoom : 0.0001f;
        const float ratio = m_zoom / startZoom;
        const float dx = m_wheelX - centerX;
        const float dy = m_wheelY - centerY;
        m_panX = dx - ((dx - m_wheelStartPanX) * ratio);
        m_panY = dy - ((dy - m_wheelStartPanY) * ratio);
    }

    if (bounds)
    {
        ClampPan(*bounds);
    }

    if (std::abs(diff) < 0.001f && std::abs(m_zoomVelocity) < 0.001f)
    {
        m_zoom = m_targetZoom;
        m_zoomVelocity = 0.0f;
        m_wheelAnchorActive = false;
        return Step::Settled;
    }
    return Step::Animating;
}

void MainImageView::ClampPan(const PanBounds& bounds)
{
    const D2D1_RECT_F& layout = bounds.layout;
    const D2D1_RECT_F& base = bounds.base;
    const float width = base.right - base.left;
    const float height = base.bottom - base.top;
    if (width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    const float scaledWidth = width * m_zoom;
    const float scaledHeight = height * m_zoom;
    if (scaledWidth <= 0.0f || scaledHeight <= 0.0f)
    {
        return;
    }

    const float centerX = (base.left + base.right) * 0.5f;
    const float centerY = (base.top + base.bottom) * 0.5f;
    constexpr float kMinVisible = 1.0f;

    float minPanX = (layout.left + kMinVisible) - (centerX + scaledWidth * 0.5f);
    float maxPanX = (layout.right - kMinVisible) - (centerX - scaledWidth * 0.5f);
    if (minPanX > maxPanX)
    {
        const float mid = (minPanX + maxPanX) * 0.5f;
        minPanX = mid;
        maxPanX = mid;
    }
    m_panX = (std::max)(minPanX, (std::min)(maxPanX, m_panX));

    float minPanY = (layout.top + kMinVisible) - (centerY + scaledHeight * 0.5f);
    float maxPanY = (layout.bottom - kMinVisible) - (centerY - scaledHeight * 0.5f);
    if (minPanY > maxPanY)
    {
        const float mid = (minPanY + maxPanY) * 0.5f;
        minPanY = mid;
        maxPanY = mid;
    }
    m_panY = (std::max)(minPanY, (std::min)(maxPanY, m_panY));
}

void MainImageView::WheelZoom(bool zoomIn, bool fine, float x, float y, unsigned long long nowMs)
{
    const float step = fine ? 0.1f : 0.5f;
    const float factor = zoomIn ? (1.0f + step) : (1.0f / (1.0f + step));
    m_wheelAnchorActive = true;
    m_wheelStartZoom = m_zoom;
    m_wheelStartPanX = m_panX;
    m_wheelStartPanY = m_panY;
    m_wheelX = x;
    m_wheelY = y;
    SetTargetZoom(m_targetZoom * factor, nowMs);
}

void MainImageView::Press(float x, float y)
{
    m_pointerDown = true;
    m_dragging = false;
    m_wheelAnchorActive = false;
    m_pressX = x;
    m_pressY = y;
    m_pressPanX = m_panX;
    m_pressPanY = m_panY;
}

MainImageView::Move MainImageView::MoveTo(float x, float y, const std::optional<PanBounds>& bounds)
{
    if (!PointerDown())
    {
        return Move::NotPressed;
    }
    const float screenDeltaX = x - m_pressX;
    const float screenDeltaY = y - m_pressY;
    if (!m_dragging &&
        (std::abs(screenDeltaX) >= kDragThresholdPx || std::abs(screenDeltaY) >= kDragThresholdPx))
    {
        m_dragging = true;
    }
    if (!m_dragging)
    {
        return Move::Held;
    }

    // The pan is applied before the rotation: undo the rotation so the
    // content follows the pointer on screen.
    float deltaX = screenDeltaX;
    float deltaY = screenDeltaY;
    if (m_rotationQuarters != 0)
    {
        RotateVectorByQuarters(screenDeltaX, screenDeltaY, (4 - m_rotationQuarters) % 4, deltaX, deltaY);
    }
    m_panX = m_pressPanX + deltaX;
    m_panY = m_pressPanY + deltaY;
    if (bounds)
    {
        ClampPan(*bounds);
    }
    m_wheelAnchorActive = false;
    return Move::Panned;
}

MainImageView::Release MainImageView::ReleasePointer()
{
    if (!PointerDown())
    {
        return Release::NotPressed;
    }
    const bool wasDragging = m_dragging;
    m_dragging = false;
    m_pointerDown = false;
    return wasDragging ? Release::DragEnd : Release::Click;
}

void MainImageView::CancelPointer()
{
    m_dragging = false;
    m_pointerDown = false;
}
