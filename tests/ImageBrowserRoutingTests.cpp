#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "ImageBrowser.h"
#include "ImageBrowserAsyncThumbLoader.h"

#include <windows.h>

#include <memory>

// Folder-listing readiness is broadcast to the window and reaches the outer
// image browser first; side-by-side browsers are nested inside it. Every
// browser must drain its own queue, or a nested pane's last chunks stay
// invisible once nothing else renders.

namespace
{
    // Must match ImageBrowserImpl::CMD_FIC2_ASYNC_THUMB_READY.
    constexpr UINT kAsyncThumbReady = WM_APP + 0x7A13;

    size_t QueuedChunks(const wchar_t* browser)
    {
        // Peek by draining and re-enqueueing (the loader has no read-only view).
        std::deque<AsyncThumbListChunkPayload> chunks = ImageBrowserAsyncThumbLoader::DequeueChunks(browser, nullptr);
        const size_t count = chunks.size();
        for (auto& chunk : chunks)
        {
            ImageBrowserAsyncThumbLoader::EnqueueChunk(browser, std::move(chunk));
        }
        return count;
    }
}

FIC2_TEST(ImageBrowserRouting_ReadyBroadcastDrainsNestedBrowser)
{
    FD2D::Backplate backplate(L"routing-test");
    auto outer = CreateImageBrowser(L"routing_outer");
    auto inner = CreateImageBrowser(L"routing_inner");
    FIC2_REQUIRE(outer && inner);
    FIC2_REQUIRE(outer->AddChild(inner));      // a side-by-side pane inside the root browser
    FIC2_REQUIRE(backplate.AddWnd(outer));

    // Rendering has stopped; the nested pane's folder delivers its final chunk.
    AsyncThumbListChunkPayload last {};
    last.completed = true;
    ImageBrowserAsyncThumbLoader::EnqueueChunk(L"routing_inner", std::move(last));
    FIC2_REQUIRE(QueuedChunks(L"routing_inner") == 1);

    // The broadcast arrives at the top-level browser, as FD2D delivers it.
    FD2D::CommandEvent event {};
    event.id = kAsyncThumbReady;
    FIC2_CHECK(outer->OnCommandEvent(event));
    FIC2_CHECK(QueuedChunks(L"routing_inner") == 0);   // the nested browser drained its queue
}
