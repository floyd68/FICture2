#include "TestHarness.h"

#include "FD2D/Backplate.h"
#include "ImageBrowserAsyncThumbLoader.h"
#include "ImageBrowserMainImage.h"
#include "SelectionStyle.h"

#include <windows.h>

#include <memory>
#include <string>

// An idle window must stop rendering: bounded selection animations, view and
// information notifications only on real changes, and folder-listing readiness
// delivered by a coalesced message instead of render-loop polling.

FIC2_TEST(IdleRendering_SelectionBreatheIsBounded)
{
    bool active = false;
    // Starts at rest and peaks mid-cycle.
    FIC2_CHECK(FD2D::SelectionBreathe(0, 900, 3, active) == 0.0f);
    FIC2_CHECK(active);
    FIC2_CHECK(FD2D::SelectionBreathe(450, 900, 3, active) > 0.99f);
    // Each whole cycle ends at rest; the last frame before the end is near rest.
    FIC2_CHECK(FD2D::SelectionBreathe(900, 900, 3, active) < 0.001f);
    FIC2_CHECK(active);
    FIC2_CHECK(FD2D::SelectionBreathe(2699, 900, 3, active) < 0.001f);
    FIC2_CHECK(active);
    // After the configured cycles: at rest, and no more frames are needed.
    FIC2_CHECK(FD2D::SelectionBreathe(2700, 900, 3, active) == 0.0f);
    FIC2_CHECK(!active);
    FIC2_CHECK(FD2D::SelectionBreathe(60000, 900, 3, active) == 0.0f);
    FIC2_CHECK(!active);
    // Disabled configurations never animate.
    FIC2_CHECK(FD2D::SelectionBreathe(100, 0, 3, active) == 0.0f && !active);
    FIC2_CHECK(FD2D::SelectionBreathe(100, 900, 0, active) == 0.0f && !active);
}

FIC2_TEST(IdleRendering_MainImageNotifiesOnlyOnRealChanges)
{
    FD2D::Backplate backplate(L"idle-test");
    auto main = std::make_shared<ImageBrowserMainImage>(L"main");
    FIC2_REQUIRE(backplate.AddWnd(main));
    main->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 200.0f, 100.0f));

    int viewNotifications = 0;
    int infoNotifications = 0;
    main->SetOnViewChanged([&](const ImageViewTransform&) { ++viewNotifications; });
    main->SetOnInfoChanged([&]() { ++infoNotifications; });

    // The D3D render pass advances the zoom animation (a null context only
    // skips the GPU draw). Settled state: repeated frames notify nothing.
    main->OnRenderD3D(nullptr);
    const int infoAfterFirst = infoNotifications;
    for (int i = 0; i < 20; ++i)
    {
        Sleep(2);
        main->OnRenderD3D(nullptr);
    }
    FIC2_CHECK(viewNotifications == 0);
    FIC2_CHECK(infoNotifications == infoAfterFirst);

    // A zoom animates: notifications while it changes, then they stop.
    main->SetZoomScale(2.0f);
    for (int i = 0; i < 400 && main->GetViewTransform().zoomScale != 2.0f; ++i)
    {
        Sleep(5);
        main->OnRenderD3D(nullptr);
    }
    FIC2_REQUIRE(main->GetViewTransform().zoomScale == 2.0f);
    const int viewAfterZoom = viewNotifications;
    const int infoAfterZoom = infoNotifications;
    FIC2_CHECK(viewAfterZoom > 0);
    FIC2_CHECK(infoAfterZoom > infoAfterFirst);   // zoom % changed

    for (int i = 0; i < 20; ++i)
    {
        Sleep(2);
        main->OnRenderD3D(nullptr);
    }
    FIC2_CHECK(viewNotifications == viewAfterZoom);
    FIC2_CHECK(infoNotifications == infoAfterZoom);
}

namespace
{
    // Counts and frees the FD2D broadcast messages posted to a message-only window.
    int DrainBroadcasts(HWND window, UINT& lastCommand)
    {
        int count = 0;
        MSG msg {};
        while (PeekMessageW(&msg, window, FD2D::Backplate::WM_FD2D_BROADCAST, FD2D::Backplate::WM_FD2D_BROADCAST, PM_REMOVE))
        {
            auto* message = reinterpret_cast<FD2D::Backplate::BroadcastMessage*>(msg.lParam);
            lastCommand = message ? message->message : 0;
            delete message;
            ++count;
        }
        return count;
    }
}

FIC2_TEST(IdleRendering_FolderChunksPostOneMessagePerBatch)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    FIC2_REQUIRE(window != nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const std::wstring browser = L"idle-test-browser";
    ImageBrowserAsyncThumbLoader::RegisterBrowser(browser, ready);
    ImageBrowserAsyncThumbLoader::SetBrowserWindow(browser, window);

    // Several chunks before the UI drains: one message.
    for (int i = 0; i < 5; ++i)
    {
        ImageBrowserAsyncThumbLoader::EnqueueChunk(browser, AsyncThumbListChunkPayload {});
    }
    UINT command = 0;
    FIC2_CHECK(DrainBroadcasts(window, command) == 1);
    FIC2_CHECK(command == WM_APP + 0x7A13);   // CMD_FIC2_ASYNC_THUMB_READY
    FIC2_CHECK(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0);

    // The UI drains; the next chunk is announced again.
    FIC2_CHECK(ImageBrowserAsyncThumbLoader::DequeueChunks(browser, ready).size() == 5);
    ImageBrowserAsyncThumbLoader::EnqueueChunk(browser, AsyncThumbListChunkPayload {});
    FIC2_CHECK(DrainBroadcasts(window, command) == 1);
    FIC2_CHECK(ImageBrowserAsyncThumbLoader::DequeueChunks(browser, ready).size() == 1);
    // Nothing queued: no message, and the drain resets the event.
    FIC2_CHECK(ImageBrowserAsyncThumbLoader::DequeueChunks(browser, ready).empty());
    FIC2_CHECK(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT);
    FIC2_CHECK(DrainBroadcasts(window, command) == 0);

    ImageBrowserAsyncThumbLoader::UnregisterBrowser(browser);
    CloseHandle(ready);
    DestroyWindow(window);
}
