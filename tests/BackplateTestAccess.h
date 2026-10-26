#pragma once

#include "FD2D/Backplate.h"

// FD2D's test access to Backplate internals, shared by the FICture2 tests
// (one definition per test executable).

namespace FD2D
{
    struct BackplateTestAccess
    {
        static void TargetRecreated(Backplate& backplate)
        {
            backplate.InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
        }

        static void DeviceLost(Backplate& backplate)
        {
            backplate.DiscardDeviceResources();
            backplate.InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        }

        // Gives a backplate in windowless render mode a (hidden) window handle
        // and nothing else: it keeps rendering into its offscreen target, but
        // Wnd::Invalidate() now renders synchronously outside a render and a
        // deferred-render scope, as in the app. Pass nullptr to detach it
        // before the window is destroyed.
        static void SetWindow(Backplate& backplate, HWND window)
        {
            backplate.m_window = window;
        }

        // Gives a windowless backplate a window and a D2D-only target the test
        // created for it (no device, no subclassing): Wnd::Invalidate() then
        // renders synchronously outside a deferred-render scope, as in the
        // app, and every render draws the backplate's children into `target`.
        static void RenderInto(Backplate& backplate, HWND window, ID2D1HwndRenderTarget* target)
        {
            RECT client {};
            GetClientRect(window, &client);
            backplate.m_window = window;
            backplate.m_size = D2D1::SizeU(
                static_cast<UINT32>(client.right - client.left), static_cast<UINT32>(client.bottom - client.top));
            backplate.m_rendererId = L"d2d_hwndrt";
            backplate.m_hwndRenderTarget = target;
            backplate.m_useOffscreenBuffer = false;
            backplate.m_layoutDirty = true;
        }
    };
}
