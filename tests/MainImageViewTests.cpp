#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "ImageBrowserMainImage.h"
#include "ImageBrowserMainImageView.h"

#include <cmath>
#include <memory>
#include <optional>

// The main image's view transform: the zoom spring, the wheel zoom anchored
// at the pointer, the left-button drag (also rotated) and the pan clamp.

namespace
{
    using Step = MainImageView::Step;
    using Move = MainImageView::Move;
    using Release = MainImageView::Release;

    bool Near(float a, float b, float tolerance = 0.01f)
    {
        return std::fabs(a - b) < tolerance;
    }

    // A 400 x 200 view showing 200 x 200 content fitted to [100, 300) x [0, 200).
    MainImageView::PanBounds Bounds()
    {
        return { D2D1::RectF(0.0f, 0.0f, 400.0f, 200.0f), D2D1::RectF(100.0f, 0.0f, 300.0f, 200.0f) };
    }

    constexpr float kCenterX = 200.0f;
    constexpr float kCenterY = 100.0f;

    // Steps the animation in 16 ms frames until it settles (or gives up).
    int Settle(MainImageView& view, unsigned long long& nowMs, const std::optional<MainImageView::PanBounds>& bounds)
    {
        for (int frame = 1; frame <= 1000; ++frame)
        {
            nowMs += 16;
            if (view.Advance(nowMs, kCenterX, kCenterY, bounds) == Step::Settled)
            {
                return frame;
            }
        }
        return -1;
    }
}

FIC2_TEST(MainImageView_ZoomTargetIsClamped)
{
    MainImageView view;
    view.SetTargetZoom(1000.0f, 1);
    FIC2_CHECK(Near(view.TargetZoom(), MainImageView::kMaxZoom));
    view.SetTargetZoom(0.0f, 1);
    FIC2_CHECK(Near(view.TargetZoom(), MainImageView::kMinZoom));
    FIC2_CHECK(Near(view.Zoom(), 1.0f));   // the shown zoom follows only by animating

    view.SetStiffness(1.0f);
    FIC2_CHECK(Near(view.Stiffness(), 10.0f));
    view.SetStiffness(10000.0f);
    FIC2_CHECK(Near(view.Stiffness(), 500.0f));
}

FIC2_TEST(MainImageView_SpringSettlesOnTheTarget)
{
    MainImageView view;
    unsigned long long now = 1000;
    view.SetTargetZoom(2.0f, now);

    // No time passed: nothing moves.
    FIC2_CHECK(view.Advance(now, kCenterX, kCenterY, std::nullopt) == Step::Idle);
    FIC2_CHECK(Near(view.Zoom(), 1.0f));

    now += 16;
    FIC2_CHECK(view.Advance(now, kCenterX, kCenterY, std::nullopt) == Step::Animating);
    FIC2_CHECK(view.Zoom() > 1.0f && view.Zoom() < 2.0f);

    const int frames = Settle(view, now, std::nullopt);
    FIC2_CHECK(frames > 0);
    FIC2_CHECK(view.Zoom() == 2.0f);
    FIC2_CHECK(view.ZoomVelocity() == 0.0f);
    // Settled stays settled.
    now += 16;
    FIC2_CHECK(view.Advance(now, kCenterX, kCenterY, std::nullopt) == Step::Settled);

    // The very first step of a fresh view starts the clock instead of
    // integrating since time 0.
    MainImageView fresh;
    FIC2_CHECK(fresh.Advance(5000, kCenterX, kCenterY, std::nullopt) == Step::Idle);
}

FIC2_TEST(MainImageView_WheelZoomKeepsThePointInPlace)
{
    MainImageView view;
    unsigned long long now = 1000;
    // The content point under (300, 150) is 100, 50 from the view center.
    view.WheelZoom(true, false, 300.0f, 150.0f, now);
    FIC2_CHECK(Near(view.TargetZoom(), 1.5f));
    FIC2_REQUIRE(Settle(view, now, std::nullopt) > 0);
    FIC2_CHECK(Near(view.Zoom(), 1.5f));
    // pan = d - d * zoom: the point stays under the pointer (computed in the
    // last step, within the settle tolerance of the zoom).
    FIC2_CHECK(Near(view.PanX(), 100.0f - 100.0f * 1.5f, 0.2f));
    FIC2_CHECK(Near(view.PanY(), 50.0f - 50.0f * 1.5f, 0.2f));

    // Out by a fine step (10%), from the target.
    view.WheelZoom(false, true, 300.0f, 150.0f, now);
    FIC2_CHECK(Near(view.TargetZoom(), 1.5f / 1.1f));

    // A drag takes over from the wheel anchor.
    view.Press(10.0f, 10.0f);
    FIC2_CHECK(view.MoveTo(20.0f, 10.0f, std::nullopt) == Move::Panned);
    const float panX = view.PanX();
    view.ReleasePointer();
    FIC2_REQUIRE(Settle(view, now, std::nullopt) > 0);
    FIC2_CHECK(Near(view.PanX(), panX));
}

FIC2_TEST(MainImageView_DragStartsPastTheThreshold)
{
    MainImageView view;
    FIC2_CHECK(view.MoveTo(50.0f, 50.0f, std::nullopt) == Move::NotPressed);
    FIC2_CHECK(view.ReleasePointer() == Release::NotPressed);

    // A press without moving far enough is a click.
    view.Press(100.0f, 100.0f);
    FIC2_CHECK(view.PointerDown());
    FIC2_CHECK(view.MoveTo(102.0f, 97.5f, std::nullopt) == Move::Held);
    FIC2_CHECK(!view.Dragging());
    FIC2_CHECK(view.ReleasePointer() == Release::Click);
    FIC2_CHECK(!view.PointerDown());
    FIC2_CHECK(view.PanX() == 0.0f && view.PanY() == 0.0f);

    // Past the threshold the content follows the pointer from the press.
    view.Press(100.0f, 100.0f);
    FIC2_CHECK(view.MoveTo(100.0f, 103.0f, std::nullopt) == Move::Panned);
    FIC2_CHECK(view.MoveTo(140.0f, 90.0f, std::nullopt) == Move::Panned);
    FIC2_CHECK(Near(view.PanX(), 40.0f) && Near(view.PanY(), -10.0f));
    FIC2_CHECK(view.ReleasePointer() == Release::DragEnd);

    // A lost capture ends the press without a click.
    view.Press(0.0f, 0.0f);
    view.CancelPointer();
    FIC2_CHECK(view.ReleasePointer() == Release::NotPressed);
}

FIC2_TEST(MainImageView_RotatedDragFollowsThePointerOnScreen)
{
    // The pan is applied before the rotation, so a screen delta is rotated
    // back: (dx, dy) on screen is (dy, -dx) at a quarter turn.
    const struct
    {
        int quarters;
        float panX;
        float panY;
    } cases[] = { { 0, 10.0f, 4.0f }, { 1, 4.0f, -10.0f }, { 2, -10.0f, -4.0f }, { 3, -4.0f, 10.0f } };
    for (const auto& c : cases)
    {
        MainImageView view;
        view.Set(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, c.quarters, 1);
        view.Press(0.0f, 0.0f);
        FIC2_CHECK(view.MoveTo(10.0f, 4.0f, std::nullopt) == Move::Panned);
        FIC2_CHECK(Near(view.PanX(), c.panX) && Near(view.PanY(), c.panY));
    }
}

FIC2_TEST(MainImageView_PanKeepsOnePixelVisible)
{
    MainImageView view;
    const MainImageView::PanBounds bounds = Bounds();

    // Zoom 1: content [100, 300) may move until one pixel is left on either side.
    view.Set(1.0f, 1.0f, 0.0f, 1000.0f, -1000.0f, 0, 1);
    view.ClampPan(bounds);
    FIC2_CHECK(Near(view.PanX(), 399.0f - 100.0f));   // left edge at 399
    FIC2_CHECK(Near(view.PanY(), 1.0f - 200.0f));     // bottom edge at 1

    // A drag is clamped the same way.
    view.Set(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0, 1);
    view.Press(0.0f, 0.0f);
    FIC2_CHECK(view.MoveTo(-1000.0f, 0.0f, bounds) == Move::Panned);
    FIC2_CHECK(Near(view.PanX(), 1.0f - 300.0f));     // right edge at 1

    // An empty content rectangle leaves the pan alone.
    view.Set(1.0f, 1.0f, 0.0f, 5000.0f, 0.0f, 0, 1);
    view.ClampPan({ D2D1::RectF(0.0f, 0.0f, 400.0f, 200.0f), D2D1::RectF(100.0f, 0.0f, 100.0f, 200.0f) });
    FIC2_CHECK(Near(view.PanX(), 5000.0f));
}

FIC2_TEST(MainImageView_SetAndNewSourceEndGestures)
{
    MainImageView view;
    view.Press(0.0f, 0.0f);
    FIC2_CHECK(view.MoveTo(10.0f, 0.0f, std::nullopt) == Move::Panned);

    view.Set(2.0f, 3.0f, 0.5f, 7.0f, 8.0f, -1, 1);
    FIC2_CHECK(view.RotationQuarters() == 3);
    FIC2_CHECK(!view.PointerDown());
    FIC2_CHECK(Near(view.Zoom(), 2.0f) && Near(view.TargetZoom(), 3.0f) && Near(view.ZoomVelocity(), 0.5f));
    FIC2_CHECK(Near(view.PanX(), 7.0f) && Near(view.PanY(), 8.0f));
    view.Set(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 6, 1);
    FIC2_CHECK(view.RotationQuarters() == 2);

    // A new source: rotation and velocity reset; zoom and pan are kept; a
    // drag in progress is reported so the caller releases the capture.
    view.Set(2.0f, 2.0f, 0.5f, 7.0f, 8.0f, 1, 1);
    view.Press(0.0f, 0.0f);
    FIC2_CHECK(view.MoveTo(10.0f, 0.0f, std::nullopt) == Move::Panned);
    FIC2_CHECK(view.ResetForNewSource(2));
    FIC2_CHECK(view.RotationQuarters() == 0);
    FIC2_CHECK(view.ZoomVelocity() == 0.0f);
    FIC2_CHECK(Near(view.Zoom(), 2.0f));
    FIC2_CHECK(!view.Dragging());
    FIC2_CHECK(!view.ResetForNewSource(3));

    // Reset zoom: back to 1 and centered; disabled interaction ends a press.
    view.ResetZoom(4);
    FIC2_CHECK(Near(view.TargetZoom(), 1.0f) && view.PanX() == 0.0f && view.PanY() == 0.0f);
    view.Press(0.0f, 0.0f);
    view.EndGestures();
    FIC2_CHECK(!view.PointerDown());
}

namespace
{
    FD2D::InputEvent Mouse(FD2D::InputEventType type, int x, int y, short wheelDelta = 0)
    {
        FD2D::InputEvent event {};
        event.type = type;
        event.button = FD2D::MouseButton::Left;
        event.point = { x, y };
        event.hasPoint = true;
        event.wheelDelta = wheelDelta;
        return event;
    }
}

// The control routes input to its view state: the wheel zooms, a press and
// release is a click, a drag pans and notifies, disabled interaction only
// clicks. (No window: there is no mouse capture to take.)
FIC2_TEST(MainImageView_ControlRoutesInput)
{
    using Type = FD2D::InputEventType;
    FD2D::Backplate backplate(L"main-image-view-test");
    auto main = std::make_shared<ImageBrowserMainImage>(L"main");
    FIC2_REQUIRE(backplate.AddWnd(main));
    main->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 200.0f, 100.0f));

    int views = 0;
    int clicks = 0;
    main->SetOnViewChanged([&](const ImageViewTransform&) { ++views; });
    main->SetOnClick([&]() { ++clicks; });

    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseWheel, 150, 50, WHEEL_DELTA)));
    FIC2_CHECK(views == 1);
    FIC2_CHECK(std::fabs(main->GetViewTransform().targetZoomScale - 1.5f) < 0.001f);
    // Outside the view the wheel is not handled.
    FIC2_CHECK(!main->OnInputEvent(Mouse(Type::MouseWheel, 500, 50, WHEEL_DELTA)));

    // Press and release in place: a click.
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseDown, 10, 10)));
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseMove, 11, 10)));
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseUp, 11, 10)));
    FIC2_CHECK(clicks == 1);
    FIC2_CHECK(views == 1);

    // A drag pans (no content yet: nothing to clamp against) and is no click.
    main->SetViewTransform(ImageViewTransform {}, false);
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseDown, 10, 10)));
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseMove, 30, 15)));
    FIC2_CHECK(views == 2);
    FIC2_CHECK(std::fabs(main->GetViewTransform().panX - 20.0f) < 0.001f);
    FIC2_CHECK(std::fabs(main->GetViewTransform().panY - 5.0f) < 0.001f);
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseUp, 30, 15)));
    FIC2_CHECK(clicks == 1);
    // Moves without a press are not the control's.
    FIC2_CHECK(!main->OnInputEvent(Mouse(Type::MouseMove, 40, 15)));

    // Rotation is normalised when set.
    ImageViewTransform rotated {};
    rotated.rotationQuarters = 5;
    main->SetViewTransform(rotated, false);
    FIC2_CHECK(main->GetViewTransform().rotationQuarters == 1);

    // Disabled interaction: a press clicks, the wheel and drags do nothing.
    main->SetInteractionEnabled(false);
    FIC2_CHECK(main->OnInputEvent(Mouse(Type::MouseDown, 10, 10)));
    FIC2_CHECK(clicks == 2);
    FIC2_CHECK(!main->OnInputEvent(Mouse(Type::MouseWheel, 150, 50, WHEEL_DELTA)));
    FIC2_CHECK(!main->OnInputEvent(Mouse(Type::MouseMove, 40, 15)));
    FIC2_CHECK(std::fabs(main->GetViewTransform().targetZoomScale - 1.0f) < 0.001f);
}
