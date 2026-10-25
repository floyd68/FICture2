#include "TestHarness.h"

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
