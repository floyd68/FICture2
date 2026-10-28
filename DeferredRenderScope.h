#pragma once

#include "FD2D/Backplate.h"

// Marks a bulk update (applying a folder listing, materializing strip tiles,
// a row-height change, restoring several viewers, committing a cached image)
// that ends in one render request.
//
// FD2D coalesces rendering (FD2D#4): Wnd::Invalidate() and
// Backplate::RequestLayout() never render synchronously; they request one
// render, which the message loop draws once per iteration after its message
// drain (or a WM_PAINT draws inside a modal loop). Any number of setters,
// invalidations and layout requests inside the scope therefore cost one
// frame, and no intermediate state is presented. The scope:
// - requests that frame when the outermost scope ends, also when nothing
//   changed (use it around updates that change the view);
// - inside an enclosing FD2D deferred-render scope (for example the file
//   drag-over batch), leaves the request to that scope's owner.
// The frame is presented by the loop, not when the scope ends: code that
// needs the arranged bounds of the update right away calls
// FD2D::Backplate::LayoutIfPending() instead of relying on a synchronous
// render; the backplate also arranges a pending layout before it routes the
// next input message.
// A null backplate (a control not attached yet) makes the scope a no-op.
// UI thread only.

class DeferredRenderScope
{
public:
    explicit DeferredRenderScope(FD2D::Backplate* backplate) noexcept
        : m_backplate(backplate)
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
            return;   // an enclosing scope requests the frame
        }
        m_backplate->RequestRender(FD2D::Backplate::RenderTrigger::Invalidate);
    }

    // A layout request for the update: marks the layout dirty and requests
    // the (coalesced) render that lays it out. Kept as the scope's entry
    // point for layout requests made during bulk updates.
    static void RequestLayout(FD2D::Backplate* backplate)
    {
        if (backplate != nullptr)
        {
            backplate->RequestLayout();
        }
    }

    DeferredRenderScope(const DeferredRenderScope&) = delete;
    DeferredRenderScope& operator=(const DeferredRenderScope&) = delete;

private:
    FD2D::Backplate* m_backplate { nullptr };
};
