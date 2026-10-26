#pragma once

#include "FD2D/Backplate.h"

#include <windows.h>

#include <algorithm>
#include <exception>
#include <vector>

// Batches the Wnd::Invalidate() and layout requests of a bulk update (applying
// a folder listing, materializing strip tiles, a row-height change, restoring
// several viewers) into one render.
//
// Outside a render and a size-move, FD2D renders and presents a frame
// synchronously on every Wnd::Invalidate() (unless a deferred-render scope is
// open) and on every Backplate::RequestLayout() (always), so a bulk update
// that runs tile setters on attached tiles cost one frame per setter. Inside
// this scope (the backplate's BeginDeferredRender / EndDeferredRender),
// Invalidate() only marks the window dirty, and a layout request made through
// DeferredRenderScope::RequestLayout() is held until the scope ends.
//
// Scopes nest (FD2D's own deferred-render scopes included); only the
// outermost one renders:
// - outside a render and a size-move, it renders the frame now, once (also
//   when nothing changed: use it around updates that change the view), with
//   the held layout request, and drops the WM_PAINT that the invalidations
//   inside the scope queued (the frame covers them). The update costs exactly
//   one frame and is on screen when the scope ends, as the synchronous
//   renders were;
// - inside a render or a size-move, every change already queued a WM_PAINT
//   (FD2D does not render re-entrantly): nothing more;
// - when the scope ends by an exception, no frame is rendered while
//   unwinding; the queued WM_PAINT is kept, and a held layout request is
//   passed on (best effort).
// A null backplate (a control not attached yet) makes the scope a no-op.
// UI thread only.
class DeferredRenderScope
{
public:
    explicit DeferredRenderScope(FD2D::Backplate* backplate) noexcept
        : m_backplate(backplate)
        , m_uncaughtExceptions(std::uncaught_exceptions())
    {
        if (m_backplate == nullptr)
        {
            return;
        }
        m_backplate->BeginDeferredRender();
        try
        {
            ++Entry(*m_backplate).depth;
        }
        catch (...)
        {
            // Without an entry, layout requests pass straight through.
        }
    }

    ~DeferredRenderScope()
    {
        if (m_backplate == nullptr)
        {
            return;
        }
        m_backplate->EndDeferredRender();

        bool layoutPending = false;
        auto& open = OpenScopes();
        const auto it = Find(*m_backplate);
        if (it != open.end())
        {
            if (--it->depth > 0)
            {
                return;   // an enclosing DeferredRenderScope renders
            }
            layoutPending = it->layoutPending;
            open.erase(it);
        }

        const bool unwinding = std::uncaught_exceptions() > m_uncaughtExceptions;
        if (m_backplate->IsDeferringRender() || unwinding)
        {
            // An enclosing FD2D scope renders, without a layout pass; or no
            // frame is rendered while unwinding.
            if (layoutPending)
            {
                try
                {
                    m_backplate->RequestLayout();
                }
                catch (...)
                {
                }
            }
            return;
        }

        const HWND window = m_backplate->Window();
        if (window == nullptr || !IsWindow(window) || m_backplate->IsRendering() || m_backplate->IsInSizeMove())
        {
            if (layoutPending)
            {
                m_backplate->RequestLayout();   // no window: only marks the layout dirty
            }
            return;
        }
        // Drop the queued paint first: what the render itself queues (a
        // recovery frame) must stay.
        (void)ValidateRect(window, nullptr);
        m_backplate->NoteRenderTrigger(FD2D::Backplate::RenderTrigger::Invalidate);
        if (layoutPending)
        {
            m_backplate->RequestLayout();   // marks the layout dirty and renders
        }
        else
        {
            m_backplate->Render();
        }
    }

    // Backplate::RequestLayout() renders synchronously outside a render and a
    // size-move, deferred-render scope or not. Inside a DeferredRenderScope
    // the request is held for the scope's render; otherwise it is made now.
    static void RequestLayout(FD2D::Backplate* backplate)
    {
        if (backplate == nullptr)
        {
            return;
        }
        const auto it = Find(*backplate);
        if (it != OpenScopes().end() && !backplate->IsRendering() && !backplate->IsInSizeMove())
        {
            it->layoutPending = true;
            return;
        }
        backplate->RequestLayout();
    }

    DeferredRenderScope(const DeferredRenderScope&) = delete;
    DeferredRenderScope& operator=(const DeferredRenderScope&) = delete;

private:
    struct OpenScope
    {
        const FD2D::Backplate* backplate { nullptr };
        int depth { 0 };
        bool layoutPending { false };
    };

    static std::vector<OpenScope>& OpenScopes()
    {
        thread_local std::vector<OpenScope> scopes;
        return scopes;
    }

    static std::vector<OpenScope>::iterator Find(const FD2D::Backplate& backplate)
    {
        auto& open = OpenScopes();
        return std::find_if(open.begin(), open.end(),
            [&](const OpenScope& scope) { return scope.backplate == &backplate; });
    }

    static OpenScope& Entry(const FD2D::Backplate& backplate)
    {
        const auto it = Find(backplate);
        if (it != OpenScopes().end())
        {
            return *it;
        }
        OpenScopes().push_back({ &backplate });
        return OpenScopes().back();
    }

    FD2D::Backplate* m_backplate { nullptr };
    int m_uncaughtExceptions { 0 };
};
