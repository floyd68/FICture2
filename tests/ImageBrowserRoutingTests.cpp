#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "FD2D/Backplate.h"
#include "ImageBrowser.h"
#include "FolderEnumeration.h"

#include <windows.h>

#include <memory>

// A folder-listing wakeup reaches the outer image browser first; side-by-side browsers are nested inside it. Every
// browser must drain its own queue, or a nested pane's last chunks stay
// invisible once nothing else renders.

FIC2_TEST(ImageBrowserRouting_ReadyBroadcastDrainsNestedBrowser)
{
    FD2D::Backplate backplate(L"routing-test");
    auto outer = CreateImageBrowser(L"routing_outer");
    auto inner = CreateImageBrowser(L"routing_inner");
    FIC2_REQUIRE(outer && inner);
    FIC2_REQUIRE(outer->AddChild(inner));      // a side-by-side pane inside the root browser
    FIC2_REQUIRE(backplate.AddWnd(outer));

    // Rendering has stopped; the nested pane's folder delivers its final chunk.
    FolderLoadLane* innerLane = ImageBrowserFolderLoadLane(*inner);
    FIC2_REQUIRE(innerLane != nullptr);
    AsyncThumbListChunkPayload last {};
    last.completed = true;
    innerLane->Post(std::move(last));
    FIC2_REQUIRE(innerLane->HasChunks());

    // The wakeup reaches the top-level browser (Ficture2Backplate dispatches it
    // to top-level Wnds only).
    FD2D::CommandEvent event {};
    event.id = kFolderListingReadyMessage;
    FIC2_CHECK(outer->OnCommandEvent(event));
    FIC2_CHECK(!innerLane->HasChunks());   // the nested browser drained its queue
}

// Deferred actions queued on several browsers in quick succession (each queue
// moves focus to its browser) all run, each on the browser that queued it,
// although the last browser keeps focus: the posted wakeups go through the
// backplate's real message routing.
FIC2_TEST(ImageBrowserRouting_DeferredActionsRunOnTheirBrowserWhicheverHasFocus)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    FIC2_REQUIRE(window != nullptr);
    {
        FD2D::Backplate backplate(L"deferred-routing-test");
        auto a = CreateImageBrowser(L"deferred_a");
        auto b = CreateImageBrowser(L"deferred_b");
        auto nested = CreateImageBrowser(L"deferred_nested");
        FIC2_REQUIRE(a && b && nested);
        FIC2_REQUIRE(b->AddChild(nested));   // a side-by-side pane inside B
        FIC2_REQUIRE(backplate.AddWnd(a));
        FIC2_REQUIRE(backplate.AddWnd(b));
        FD2D::BackplateTestAccess::SetWindow(backplate, window);

        auto ops = [](const std::shared_ptr<FD2D::Wnd>& browser)
        {
            return dynamic_cast<IImageBrowserOps*>(browser.get());
        };
        FIC2_REQUIRE(ops(a) && ops(b) && ops(nested));

        ops(a)->BrowserCmdNavigateUp();
        ops(a)->BrowserCmdNavigateUp();
        ops(nested)->BrowserCmdNavigateUp();
        ops(b)->BrowserCmdNavigateUp();
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*a) == 2);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*nested) == 1);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*b) == 1);
        FIC2_CHECK(backplate.FocusedWnd() == b.get());

        // One wakeup per action; each runs one action on its own browser.
        size_t wakeups = 0;
        MSG msg {};
        while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE))
        {
            ++wakeups;
            (void)FD2D::BackplateTestAccess::HandleMessage(backplate, window, msg.message, msg.wParam, msg.lParam);
        }
        FIC2_CHECK(wakeups == 4);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*a) == 0);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*nested) == 0);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*b) == 0);

        FD2D::BackplateTestAccess::SetWindow(backplate, nullptr);
    }
    DestroyWindow(window);
}
