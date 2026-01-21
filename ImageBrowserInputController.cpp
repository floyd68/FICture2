#include "ImageBrowserInputController.h"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace
{
    bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
    {
        return pt.x >= r.left &&
            pt.x <= r.right &&
            pt.y >= r.top &&
            pt.y <= r.bottom;
    }
}

bool ImageBrowserInputController::HandleMouseWheel(const MouseWheelContext& ctx, WPARAM wParam, LPARAM lParam)
{
    const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    if (ctx.hasMainRect && RectContainsPoint(ctx.mainRect, pt))
    {
        if (ctx.requestFocus)
        {
            ctx.requestFocus();
        }
    }

    if (ctx.hasThumbRect && ctx.items && !ctx.items->empty() && RectContainsPoint(ctx.thumbRect, pt))
    {
        if (ctx.requestFocus)
        {
            ctx.requestFocus();
        }

        m_thumbWheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = m_thumbWheelRemainder / WHEEL_DELTA;
        m_thumbWheelRemainder = m_thumbWheelRemainder % WHEEL_DELTA;

        if (steps != 0)
        {
            const int dir = (steps > 0) ? -1 : 1;
            const int count = std::abs(steps);

            size_t cur = ctx.selectedIndex;
            size_t next = cur;

            for (int s = 0; s < count; ++s)
            {
                size_t probe = next;
                bool found = false;
                while (true)
                {
                    if (dir < 0)
                    {
                        if (probe == 0)
                        {
                            break;
                        }
                        probe--;
                    }
                    else
                    {
                        probe++;
                        if (probe >= ctx.items->size())
                        {
                            break;
                        }
                    }

                    if ((*ctx.items)[probe].kind == ThumbItemKind::Image)
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    break;
                }
                next = probe;
            }

            if (next != cur && ctx.selectIndex)
            {
                ctx.selectIndex(next);
            }
        }

        return true;
    }

    return false;
}

bool ImageBrowserInputController::HandleKeyDown(const KeyContext& ctx, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(message);
    UNREFERENCED_PARAMETER(lParam);

    const size_t cur = (ctx.items && ctx.selectedIndex < ctx.items->size()) ? ctx.selectedIndex : 0;

    switch (wParam)
    {
    case VK_LEFT:
        {
            const size_t next = (cur == 0) ? 0 : (cur - 1);
            if (ctx.selectIndex)
            {
                ctx.selectIndex(next);
            }
        }
        return true;
    case VK_RIGHT:
        if (ctx.selectIndex)
        {
            ctx.selectIndex(cur + 1);
        }
        return true;
    default:
        return false;
    }
}

bool ImageBrowserInputController::HandleKeyUp(const KeyContext& ctx, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(message);
    UNREFERENCED_PARAMETER(lParam);

    const bool ctrl = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
    const bool shift = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
    const bool alt = ((GetKeyState(VK_MENU) & 0x8000) != 0);

    auto PageStep = [&ctx]() -> size_t
    {
        if (!ctx.hasThumbRect)
        {
            return 1;
        }
        const float w = ctx.thumbRect.right - ctx.thumbRect.left;
        if (w <= 1.0f)
        {
            return 1;
        }
        const float itemExtent = (std::max)(1.0f, ctx.thumbW + ctx.thumbOuterSpacing);
        const int count = static_cast<int>(std::floor(w / itemExtent));
        return static_cast<size_t>((std::max)(1, count));
    };

    switch (wParam)
    {
    case VK_F4:
        if (ctrl && ctx.closeHorizontalThisBrowser)
        {
            ctx.closeHorizontalThisBrowser();
            return true;
        }
        return false;
    case VK_UP:
        if (!alt)
        {
            return false;
        }
    case VK_BACK:
        if (ctx.queueNavigateUp)
        {
            ctx.queueNavigateUp();
        }
        return true;
    case 'N':
        if (ctx.queueToggleNavItems)
        {
            ctx.queueToggleNavItems();
        }
        return true;
    case 'A':
    case 'a':
        if (ctx.toggleAlphaCheckerboard)
        {
            ctx.toggleAlphaCheckerboard();
        }
        return true;
    case 'X':
    case 'x':
        if (ctx.fitToScreen)
        {
            ctx.fitToScreen();
        }
        return true;
    case 'B':
    case 'b':
        if (ctx.pickBackgroundColor)
        {
            ctx.pickBackgroundColor();
        }
        return true;
    case 'Q':
    case 'q':
        if (ctx.toggleSamplingQuality)
        {
            ctx.toggleSamplingQuality();
        }
        return true;
    case VK_RETURN:
        if (ctx.items && !ctx.items->empty() && ctx.selectedIndex < ctx.items->size())
        {
            if (ctx.activateSelected)
            {
                ctx.activateSelected();
            }
            return true;
        }
        return true;
    case 'O':
    case 'o':
        if (ctrl && shift)
        {
            if (ctx.openFileSplit)
            {
                ctx.openFileSplit();
            }
            return true;
        }
        if (ctrl)
        {
            if (ctx.openFileReplace)
            {
                ctx.openFileReplace();
            }
            return true;
        }
        return false;
    case VK_HOME:
        if (ctx.selectIndex)
        {
            ctx.selectIndex(0);
        }
        return true;
    case VK_END:
        if (ctx.items && ctx.selectIndex)
        {
            ctx.selectIndex(ctx.items->size() - 1);
        }
        return true;
    case VK_PRIOR:
        {
            const size_t step = PageStep();
            const size_t cur = (ctx.items && ctx.selectedIndex < ctx.items->size()) ? ctx.selectedIndex : 0;
            const size_t next = (cur > step) ? (cur - step) : 0;
            if (ctx.selectIndex)
            {
                ctx.selectIndex(next);
            }
        }
        return true;
    case VK_NEXT:
        {
            const size_t step = PageStep();
            const size_t cur = (ctx.items && ctx.selectedIndex < ctx.items->size()) ? ctx.selectedIndex : 0;
            size_t next = cur + step;
            if (ctx.selectIndex)
            {
                ctx.selectIndex(next);
            }
        }
        return true;
    default:
        return false;
    }
}
