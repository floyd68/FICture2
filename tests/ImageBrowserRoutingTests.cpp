#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "FD2D/Backplate.h"
#include "Ficture2Backplate.h"
#include "ImageBrowser.h"
#include "FolderEnumeration.h"
#include "ThumbTestSupport.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // Runs the thread's posted messages as the app's message loop does: the
    // backplate's through its message routing, the rest (deferred-action
    // wakeups) through their own window procedures.
    void PumpMessages(FD2D::Backplate& backplate, HWND window)
    {
        MSG msg {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.hwnd == window)
            {
                (void)FD2D::BackplateTestAccess::HandleMessage(backplate, window, msg.message, msg.wParam, msg.lParam);
            }
            else
            {
                DispatchMessageW(&msg);
            }
        }
    }

    IImageBrowserOps* Ops(FD2D::Wnd* browser)
    {
        return dynamic_cast<IImageBrowserOps*>(browser);
    }
}

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
// although the last browser keeps focus.
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
        FIC2_REQUIRE(Ops(a.get()) && Ops(b.get()) && Ops(nested.get()));

        Ops(a.get())->BrowserCmdNavigateUp();
        Ops(a.get())->BrowserCmdNavigateUp();
        Ops(nested.get())->BrowserCmdNavigateUp();
        Ops(b.get())->BrowserCmdNavigateUp();
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*a) == 2);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*nested) == 1);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*b) == 1);
        FIC2_CHECK(backplate.FocusedWnd() == b.get());

        // No action runs inside the backplate's routing (a recursive walk of
        // the Wnd tree that a structural action would rebuild under it).
        MSG msg {};
        while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE))
        {
            (void)FD2D::BackplateTestAccess::HandleMessage(backplate, window, msg.message, msg.wParam, msg.lParam);
        }
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*a) == 2);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*nested) == 1);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*b) == 1);

        PumpMessages(backplate, window);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*a) == 0);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*nested) == 0);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*b) == 0);

        FD2D::BackplateTestAccess::SetWindow(backplate, nullptr);
    }
    DestroyWindow(window);
}

// Structural actions queued back to back: two folders dropped on the insert
// edge before the first insert ran, then both added panes closed before either
// close ran. Each one rebuilds the root's split tree, destroying the split
// panels and (for a close) the browser that queued it, so it must run outside
// any Wnd-tree traversal with its browser retained.
FIC2_TEST(ImageBrowserRouting_QueuedPaneInsertsAndClosesAllRun)
{
    wchar_t tempPath[MAX_PATH] {};
    GetTempPathW(MAX_PATH, tempPath);
    const std::filesystem::path root = std::filesystem::path(tempPath) /
        (L"FICture2Tests_DeferredSplit_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    FIC2_REQUIRE(std::filesystem::create_directories(root / L"one") && std::filesystem::create_directories(root / L"two"));

    ThumbTestSupport::ComScope com;
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    FIC2_REQUIRE(window != nullptr);
    {
        Ficture2Backplate backplate(L"deferred-split-test");
        backplate.SkipImageBrowserIniForTesting();
        FD2D::BackplateTestAccess::SetWindow(backplate, window);
        auto host = CreateImageBrowser(L"deferred_split_root");
        FIC2_REQUIRE(host != nullptr);
        FIC2_REQUIRE(backplate.AddWnd(host));
        auto layout = [&]
        {
            (void)host->Measure({ 1280.0f, 720.0f });
            host->Arrange({ 0.0f, 0.0f, 1280.0f, 720.0f });
        };
        layout();

        D2D1_RECT_F main {};
        FIC2_REQUIRE(Ops(host.get())->BrowserTryGetMainImageClientRect(main));
        const POINT insertEdge { static_cast<LONG>(main.right) - 4, static_cast<LONG>((main.top + main.bottom) / 2) };
        FIC2_REQUIRE(host->OnFileDrop((root / L"one").wstring(), insertEdge));
        FIC2_REQUIRE(host->OnFileDrop((root / L"two").wstring(), insertEdge));
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*host) == 2);

        PumpMessages(backplate, window);
        layout();
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*host) == 0);
        FIC2_REQUIRE(backplate.Registry().ImageBrowserCount() == 3);

        std::vector<FD2D::Wnd*> added;
        for (FD2D::Wnd* browser : backplate.Registry().ImageBrowsersSnapshot())
        {
            if (browser != host.get())
            {
                added.push_back(browser);
            }
        }
        FIC2_REQUIRE(added.size() == 2);
        for (FD2D::Wnd* browser : added)
        {
            FIC2_REQUIRE(Ops(browser) != nullptr);
            Ops(browser)->BrowserCmdClose();
            FIC2_CHECK(ImageBrowserQueuedDeferredActions(*browser) == 1);
        }

        PumpMessages(backplate, window);
        layout();
        FIC2_CHECK(backplate.Registry().ImageBrowserCount() == 1);
        FIC2_CHECK(ImageBrowserQueuedDeferredActions(*host) == 0);

        FD2D::BackplateTestAccess::SetWindow(backplate, nullptr);
    }
    DestroyWindow(window);
    std::filesystem::remove_all(root, ec);
}
