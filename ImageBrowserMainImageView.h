#pragma once

#include <d2d1.h>

#include <optional>

// The main image's view transform and the gestures that change it, without
// UI objects: zoom (a damped spring toward a target), pan, rotation, the
// left-button drag and the wheel zoom anchored at the pointer. Times are in
// milliseconds and points in client coordinates; the caller captures the
// mouse, requests frames, invalidates and notifies.
class MainImageView
{
public:
    static constexpr float kMinZoom = 0.1f;
    static constexpr float kMaxZoom = 50.0f;
    // A drag starts once the pointer moved this far from where it was pressed.
    static constexpr float kDragThresholdPx = 3.0f;

    // What a pan is clamped against: the view's layout rectangle and the
    // content fitted into it at zoom 1 (centered, aspect kept).
    struct PanBounds
    {
        D2D1_RECT_F layout {};
        D2D1_RECT_F base {};
    };

    float Zoom() const { return m_zoom; }
    float TargetZoom() const { return m_targetZoom; }
    float ZoomVelocity() const { return m_zoomVelocity; }
    float Stiffness() const { return m_stiffness; }
    float PanX() const { return m_panX; }
    float PanY() const { return m_panY; }
    int RotationQuarters() const { return m_rotationQuarters; }
    // The left button is down on the view (a click or a drag).
    bool PointerDown() const { return m_pointerDown || m_dragging; }
    bool Dragging() const { return m_dragging; }

    // Sets the whole transform (a synchronized pane, a rotation): rotation is
    // normalized to 0-3 quarters, gestures end and the animation clock restarts.
    void Set(float zoom, float targetZoom, float zoomVelocity, float panX, float panY,
        int rotationQuarters, unsigned long long nowMs);
    // The zoom the animation moves toward, clamped to [kMinZoom, kMaxZoom].
    void SetTargetZoom(float zoom, unsigned long long nowMs);
    // Back to zoom 1, centered, without a drag or wheel anchor.
    void ResetZoom(unsigned long long nowMs);
    void SetStiffness(float stiffness);
    // A different source is shown: rotation, the wheel anchor and the zoom
    // velocity reset. Returns true if a drag was in progress (it ends; the
    // caller releases the mouse capture).
    bool ResetForNewSource(unsigned long long nowMs);
    // Interaction is disabled: the press, the drag and the wheel anchor end.
    void EndGestures();

    enum class Step
    {
        Idle,        // no time passed since the last step
        Animating,   // still moving toward the target
        Settled,     // at the target (and at rest)
    };
    // One animation step at `nowMs`. A wheel zoom keeps its anchor point in
    // place relative to the view center (`centerX`, `centerY`); the pan is
    // then clamped to `bounds` (when known).
    Step Advance(unsigned long long nowMs, float centerX, float centerY, const std::optional<PanBounds>& bounds);

    // Keeps at least one pixel of the zoomed content inside the view; content
    // that cannot cover the range is centered on the clamp range's middle.
    void ClampPan(const PanBounds& bounds);

    // A wheel notch at (x, y): the target zooms in or out by 50% (10% when
    // `fine`), anchored at the pointer until the animation settles.
    void WheelZoom(bool zoomIn, bool fine, float x, float y, unsigned long long nowMs);

    // The left button is pressed on the view at (x, y).
    void Press(float x, float y);
    enum class Move
    {
        NotPressed,   // no press in progress
        Held,         // pressed, but not past the drag threshold
        Panned,       // the pan follows the pointer
    };
    // The pointer moved to (x, y). A drag moves the content with the pointer
    // on screen, whatever the rotation.
    Move MoveTo(float x, float y, const std::optional<PanBounds>& bounds);
    enum class Release
    {
        NotPressed,
        Click,     // released without dragging
        DragEnd,
    };
    Release ReleasePointer();
    // The mouse capture was lost: the press or drag ends without a click.
    void CancelPointer();

private:
    float m_zoom { 1.0f };
    float m_targetZoom { 1.0f };
    float m_zoomVelocity { 0.0f };
    unsigned long long m_lastStepMs { 0 };
    float m_stiffness { 100.0f };

    float m_panX { 0.0f };
    float m_panY { 0.0f };
    int m_rotationQuarters { 0 };

    bool m_dragging { false };
    bool m_pointerDown { false };
    float m_pressX { 0.0f };
    float m_pressY { 0.0f };
    float m_pressPanX { 0.0f };
    float m_pressPanY { 0.0f };

    bool m_wheelAnchorActive { false };
    float m_wheelStartZoom { 1.0f };
    float m_wheelStartPanX { 0.0f };
    float m_wheelStartPanY { 0.0f };
    float m_wheelX { 0.0f };
    float m_wheelY { 0.0f };
};
