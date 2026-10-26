#pragma once

#include "FD2D/Backplate.h"

#include <windows.h>

#include <exception>

// Batches the Wnd::Invalidate() calls of a bulk update (applying a folder
// listing, materializing strip tiles, a row-height change, restoring several
// viewers) into one render.
//
// Outside a render, a size-move or a deferred-render scope, FD2D's
// Wnd::Invalidate() renders and presents a frame synchronously, so a bulk
// update that runs tile setters on attached tiles would cost one frame per
// setter. Inside this scope (the backplate's BeginDeferredRender /
// EndDeferredRender) Invalidate() only marks the window dirty.
//
// Scopes nest through the backplate's depth counter (FD2D's own scopes
// included); only the outermost one requests the render:
// - outside a render and a size-move, it renders the frame now, once (also
//   when nothing changed: use it around updates that change the view), and
//   drops the WM_PAINT that the invalidations inside the scope queued (the
//   frame it renders covers them), so the update costs exactly one frame and
//   is on screen when the scope ends, as the synchronous renders were;
// - inside a render or a size-move, Invalidate() already queued a WM_PAINT for
//   every change, and FD2D does not render re-entrantly: nothing more;
// - when the scope ends by an exception, the queued WM_PAINT is kept and no
//   render runs while unwinding.
// A null backplate (a control not attached yet) makes the scope a no-op.
class DeferredRenderScope
{
public:
    explicit DeferredRenderScope(FD2D::Backplate* backplate) noexcept
        : m_backplate(backplate)
        , m_uncaughtExceptions(std::uncaught_exceptions())
    {
        if (m_backplate != nullptr)
        {
            m_backplate->BeginDeferredRender();
        }
    }

    ~DeferredRenderScope()
    {
        if (m_backplate == nullptr)
        {
            return;
        }
        m_backplate->EndDeferredRender();
        if (m_backplate->IsDeferringRender())
        {
            return;   // an enclosing scope renders
        }
        if (std::uncaught_exceptions() > m_uncaughtExceptions)
        {
            return;
        }

        const HWND window = m_backplate->Window();
        if (window == nullptr || !IsWindow(window))
        {
            return;
        }
        if (m_backplate->IsRendering() || m_backplate->IsInSizeMove())
        {
            return;
        }
        // Drop the queued paint first: what Render() itself queues (a
        // recovery frame) must stay.
        (void)ValidateRect(window, nullptr);
        m_backplate->NoteRenderTrigger(FD2D::Backplate::RenderTrigger::Invalidate);
        m_backplate->Render();
    }

    DeferredRenderScope(const DeferredRenderScope&) = delete;
    DeferredRenderScope& operator=(const DeferredRenderScope&) = delete;

private:
    FD2D::Backplate* m_backplate { nullptr };
    int m_uncaughtExceptions { 0 };
};
