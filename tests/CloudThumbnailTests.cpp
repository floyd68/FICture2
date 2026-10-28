#include "TestHarness.h"

#include "CloudThumbnailRouter.h"
#include "FloarPathByteSource.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserThumbnailPane.h"
#include "ShellThumbnailSource.h"
#include "ThumbImageTile.h"
#include "ThumbTestSupport.h"
#include "CommonUtil.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Shell thumbnails of online-only cloud files (#59): CloudThumbnailRouter in
// front of the decode service, with a faked placeholder query and a faked
// shell source (no file, network or shell access), and the HBITMAP payload
// conversion on DIB sections made in the test.

namespace
{
    using namespace std::chrono_literals;

    const std::wstring kCloud = L"C:\\Cloud\\";   // every file under it is online-only
    const std::wstring kLocal = L"C:\\Local\\";

    // C: is a fixed drive; files under kCloud report
    // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, others under C: are plain files.
    class FakeQueries final : public PathClassificationQueries
    {
    public:
        std::uint32_t DriveType(const std::wstring& root) const override
        {
            return (!root.empty() && root[0] == L'C') ? static_cast<std::uint32_t>(DRIVE_FIXED)
                                                      : static_cast<std::uint32_t>(DRIVE_NO_ROOT_DIR);
        }

        // Case-insensitive, as the file system (tiles bind lower-cased paths).
        std::uint32_t FileAttributes(const std::wstring& path) const override
        {
            const std::wstring lower = CommonUtil::ToLower(path);
            if (lower.rfind(CommonUtil::ToLower(kCloud), 0) == 0)
            {
                return FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
            }
            return lower.rfind(L"c:\\", 0) == 0 ? FILE_ATTRIBUTE_ARCHIVE : INVALID_FILE_ATTRIBUTES;
        }
    };

    // Answers with `respond(path, width, height)` (a Ready thumbnail of the
    // box by default) and records every call. While held, calls block until
    // Release().
    class FakeShellSource final : public IShellThumbnailSource
    {
    public:
        struct Call
        {
            std::wstring path;
            uint32_t width { 0 };
            uint32_t height { 0 };
        };

        std::function<ShellThumbnailResult(const std::wstring&, uint32_t, uint32_t)> respond;

        ShellThumbnailResult GetThumbnail(const std::wstring& path, uint32_t width, uint32_t height) override
        {
            std::function<ShellThumbnailResult(const std::wstring&, uint32_t, uint32_t)> responder;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_calls.push_back({ path, width, height });
                m_changed.notify_all();
                m_changed.wait(lock, [this] { return !m_held; });
                responder = respond;
            }
            return responder ? responder(path, width, height) : Ready(width, height);
        }

        static ShellThumbnailResult Ready(uint32_t width, uint32_t height)
        {
            ShellThumbnailResult result {};
            result.status = ShellThumbnailStatus::Ready;
            result.hr = S_OK;
            result.image = ThumbTestSupport::MakeBgraImage(width, height);
            return result;
        }

        static ShellThumbnailResult Status(ShellThumbnailStatus status, HRESULT hr)
        {
            ShellThumbnailResult result {};
            result.status = status;
            result.hr = hr;
            return result;
        }

        void Hold()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_held = true;
        }

        void Release()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_held = false;
            m_changed.notify_all();
        }

        // Waits until `count` calls were made in all.
        bool WaitForCalls(size_t count, std::chrono::milliseconds timeout = 5s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_calls.size() >= count; });
        }

        std::vector<Call> Calls() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_calls;
        }

        size_t CallsFor(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<size_t>(std::count_if(m_calls.begin(), m_calls.end(),
                [&](const Call& call) { return call.path == path; }));
        }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_changed;
        std::vector<Call> m_calls;
        bool m_held { false };
    };

    // Short polls and deadline, so pending thumbnails resolve quickly.
    CloudThumbnailRouter::Options FastOptions(size_t workers = 2)
    {
        CloudThumbnailRouter::Options options {};
        options.workers = workers;
        options.firstPoll = 1ms;
        options.maxPoll = 4ms;
        options.pendingDeadline = 300ms;
        return options;
    }

    template <typename Predicate>
    bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 5s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() > end)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    ImageCore::ImageRequest ThumbRequest(const std::wstring& path, float width = 128.0f, float height = 128.0f)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::Thumbnail);
        request.targetSize = { width, height };
        request.allowGpuCompressedDDS = false;
        return request;
    }

    // A router over a recording inner service, installed as the bindings'
    // decode service for the life of the fixture.
    struct RouterFixture
    {
        ThumbTestSupport::RecordingDecodeService inner {};
        FakeQueries queries {};
        FakeShellSource shell {};
        std::unique_ptr<CloudThumbnailRouter> router {};

        explicit RouterFixture(CloudThumbnailRouter::Options options = FastOptions())
            : router(std::make_unique<CloudThumbnailRouter>(inner, shell, &queries, options))
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(router.get());
        }

        ~RouterFixture()
        {
            shell.Release();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
            router.reset();
        }

        bool Idle() const { return router->OutstandingShellRequests() == 0; }
    };

    bool WaitForPhase(const ImageAsyncBinding& binding, ImageAsyncBinding::Phase phase)
    {
        return WaitUntil([&] { return binding.CurrentPhase() == phase; });
    }

    // A 32 bpp DIB section of `width` x `height`, top-down when `topDown`,
    // filled from `pixel(x, y)` (BGRA, in display row order).
    HBITMAP MakeDib(int width, int height, bool topDown, WORD bitCount,
        const std::function<std::array<uint8_t, 4>(int, int)>& pixel)
    {
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = topDown ? -height : height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = bitCount;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        FIC2_REQUIRE(bitmap != nullptr && bits != nullptr);
        const int bytesPerPixel = bitCount / 8;
        const int stride = ((width * bitCount + 31) / 32) * 4;
        for (int y = 0; y < height; ++y)
        {
            const int memoryRow = topDown ? y : (height - 1 - y);
            uint8_t* row = static_cast<uint8_t*>(bits) + static_cast<size_t>(memoryRow) * stride;
            for (int x = 0; x < width; ++x)
            {
                const std::array<uint8_t, 4> value = pixel(x, y);
                for (int c = 0; c < bytesPerPixel; ++c)
                {
                    row[x * bytesPerPixel + c] = value[static_cast<size_t>(c)];
                }
            }
        }
        GdiFlush();
        return bitmap;
    }

    std::array<uint8_t, 4> PixelOf(const ImageCore::DecodedImage& image, uint32_t x, uint32_t y)
    {
        const uint8_t* p = image.blocks->data() + static_cast<size_t>(y) * image.rowPitchBytes + x * 4u;
        return { p[0], p[1], p[2], p[3] };
    }
}

// Routing: a thumbnail of an online-only file comes from the shell, at the
// request box, and the byte path is never asked; a local file's thumbnail, and
// the main image of an online-only file (which must download), take the byte
// path and never reach the shell.
FIC2_TEST(CloudThumbnail_RoutesOnlineOnlyThumbnailsToTheShell)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture;
    const std::wstring cloud = kCloud + L"a.jpg";
    const std::wstring local = kLocal + L"b.jpg";
    fixture.shell.respond = [](const std::wstring&, uint32_t, uint32_t) { return FakeShellSource::Ready(120, 90); };

    FIC2_CHECK(fixture.router->RoutesToShell(ThumbRequest(cloud)));
    FIC2_CHECK(!fixture.router->RoutesToShell(ThumbRequest(local)));
    FIC2_CHECK(!fixture.router->RoutesToShell(ThumbRequest(kCloud + L"pack.zip\\c.jpg")));   // archive entry

    ImageAsyncBinding online;
    online.RequestLoad(ThumbRequest(cloud, 170.4f, 128.0f), cloud, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(WaitForPhase(online, ImageAsyncBinding::Phase::Ready));
    const ImageAsyncBinding::Payload payload = online.TakePending();
    FIC2_CHECK(payload.width == 120 && payload.height == 90);
    FIC2_CHECK(payload.sourcePath == CommonUtil::NormalizePath(cloud));
    const std::vector<FakeShellSource::Call> calls = fixture.shell.Calls();
    FIC2_REQUIRE(calls.size() == 1);
    FIC2_CHECK(calls[0].path == cloud);
    FIC2_CHECK(calls[0].width == 171 && calls[0].height == 128);   // the box, rounded up
    FIC2_CHECK(fixture.inner.RequestCount() == 0);

    ImageAsyncBinding plain;
    plain.RequestLoad(ThumbRequest(local), local, ImageCore::RequestPriority::Visible);
    ImageAsyncBinding mainImage;
    ImageCore::ImageRequest full(cloud, ImageCore::ImagePurpose::FullResolution);
    mainImage.RequestLoad(full, cloud);
    const auto requests = fixture.inner.Requests();
    FIC2_REQUIRE(requests.size() == 2);
    FIC2_CHECK(requests[0].request.source == local);
    FIC2_CHECK(requests[0].request.purpose == ImageCore::ImagePurpose::Thumbnail);
    FIC2_CHECK(requests[1].request.source == cloud);
    FIC2_CHECK(requests[1].request.purpose == ImageCore::ImagePurpose::FullResolution);
    FIC2_CHECK(fixture.shell.Calls().size() == 1);
    FIC2_CHECK(fixture.router->GetStats().routed == 1);
}

// Fallback: a shell failure, or a thumbnail still pending at the deadline,
// sends the same request to the byte path at its current priority; a
// thumbnail that becomes ready while polled is published without the byte
// path. The caller's thread never waits for a shell call.
FIC2_TEST(CloudThumbnail_ShellFailureFallsBackToTheBytePath)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture;
    const std::wstring failing = kCloud + L"failing.dds";
    const std::wstring stuck = kCloud + L"stuck.jpg";
    const std::wstring slow = kCloud + L"slow.jpg";
    auto pendingCalls = std::make_shared<std::map<std::wstring, int>>();
    auto mutex = std::make_shared<std::mutex>();
    fixture.shell.respond = [=](const std::wstring& path, uint32_t, uint32_t)
    {
        if (path == failing)
        {
            return FakeShellSource::Status(ShellThumbnailStatus::Failed, E_FAIL);
        }
        if (path == stuck)
        {
            return FakeShellSource::Status(ShellThumbnailStatus::Pending, static_cast<HRESULT>(0x8004B205));
        }
        std::lock_guard<std::mutex> lock(*mutex);
        if (++(*pendingCalls)[path] < 3)
        {
            return FakeShellSource::Status(ShellThumbnailStatus::Pending, static_cast<HRESULT>(0x8004B205));
        }
        return FakeShellSource::Ready(64, 48);
    };

    // The caller returns while the shell call is held.
    fixture.shell.Hold();
    ImageAsyncBinding a;
    const auto before = std::chrono::steady_clock::now();
    a.RequestLoad(ThumbRequest(failing), failing, ImageCore::RequestPriority::Prefetch);
    FIC2_CHECK(std::chrono::steady_clock::now() - before < 1s);
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));
    FIC2_CHECK(a.CurrentPhase() == ImageAsyncBinding::Phase::Loading);
    a.SetPriority(ImageCore::RequestPriority::Visible);
    fixture.shell.Release();

    FIC2_REQUIRE(WaitUntil([&] { return fixture.inner.RequestCount() == 1; }));
    auto requests = fixture.inner.Requests();
    FIC2_CHECK(requests[0].request == ThumbRequest(failing));
    FIC2_CHECK(requests[0].priority == ImageCore::RequestPriority::Visible);
    // Its completion reaches the binding as an ImageCore completion would.
    fixture.inner.Complete(requests[0].handle, S_OK, ThumbTestSupport::MakeBgraImage(128, 128));
    FIC2_REQUIRE(WaitForPhase(a, ImageAsyncBinding::Phase::Ready));
    FIC2_CHECK(a.TakePending().width == 128);

    ImageAsyncBinding b;
    b.RequestLoad(ThumbRequest(stuck), stuck, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(WaitUntil([&] { return fixture.inner.RequestCount() == 2; }));
    FIC2_CHECK(fixture.shell.CallsFor(stuck) >= 2);   // polled until the deadline
    FIC2_CHECK(fixture.inner.Requests()[1].request.source == stuck);

    ImageAsyncBinding c;
    c.RequestLoad(ThumbRequest(slow), slow, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(WaitForPhase(c, ImageAsyncBinding::Phase::Ready));
    FIC2_CHECK(c.TakePending().width == 64);
    FIC2_CHECK(fixture.shell.CallsFor(slow) == 3);
    FIC2_CHECK(fixture.inner.RequestCount() == 2);

    const CloudThumbnailRouter::Stats stats = fixture.router->GetStats();
    FIC2_CHECK(stats.routed == 3);
    FIC2_CHECK(stats.fallbacks == 2);
    FIC2_CHECK(stats.ready == 1);
    FIC2_CHECK(fixture.Idle());
}

// Cancellation before the shell call starts: the request leaves the queue,
// the shell and the byte path never see it, and nothing is published. A
// request canceled during its shell call publishes nothing and does not fall
// back, even when the shell fails.
FIC2_TEST(CloudThumbnail_CancelBeforeStartPublishesNothing)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    const std::wstring running = kCloud + L"running.jpg";
    const std::wstring queued = kCloud + L"queued.jpg";
    fixture.shell.respond = [](const std::wstring&, uint32_t, uint32_t)
    {
        return FakeShellSource::Status(ShellThumbnailStatus::Failed, E_FAIL);
    };

    fixture.shell.Hold();
    ImageAsyncBinding first;
    first.RequestLoad(ThumbRequest(running), running, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));   // the only worker is in its call
    ImageAsyncBinding second;
    second.RequestLoad(ThumbRequest(queued), queued, ImageCore::RequestPriority::Visible);
    FIC2_CHECK(fixture.router->OutstandingShellRequests() == 2);

    second.Cancel();
    FIC2_CHECK(fixture.router->OutstandingShellRequests() == 1);
    first.Cancel();
    FIC2_CHECK(fixture.Idle());
    fixture.shell.Release();

    std::this_thread::sleep_for(50ms);
    FIC2_CHECK(fixture.shell.CallsFor(queued) == 0);
    FIC2_CHECK(fixture.shell.CallsFor(running) == 1);
    FIC2_CHECK(fixture.inner.RequestCount() == 0);   // no fallback for either
    FIC2_CHECK(first.CurrentPhase() == ImageAsyncBinding::Phase::Superseded);
    FIC2_CHECK(second.CurrentPhase() == ImageAsyncBinding::Phase::Superseded);
    const CloudThumbnailRouter::Stats stats = fixture.router->GetStats();
    FIC2_CHECK(stats.canceled == 2);
    FIC2_CHECK(stats.fallbacks == 0 && stats.ready == 0);

    // The router itself publishes nothing for a canceled handle either.
    bool called = false;
    fixture.shell.Hold();
    const ImageCore::ImageHandle blocker = fixture.router->RequestDecoded(ThumbRequest(running),
        [](HRESULT, ImageCore::DecodedImage) {}, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(fixture.shell.WaitForCalls(2));
    const ImageCore::ImageHandle handle = fixture.router->RequestDecoded(ThumbRequest(queued),
        [&called](HRESULT, ImageCore::DecodedImage) { called = true; }, ImageCore::RequestPriority::Visible);
    FIC2_CHECK((handle & CloudThumbnailRouter::kShellHandleBit) != 0);
    fixture.router->Cancel(handle);
    fixture.router->Cancel(blocker);
    fixture.shell.Release();
    std::this_thread::sleep_for(50ms);
    FIC2_CHECK(!called);
    FIC2_CHECK(fixture.shell.CallsFor(queued) == 0);
}

// Visible thumbnails are asked for before prefetch ones, oldest first.
FIC2_TEST(CloudThumbnail_VisibleRequestsRunBeforePrefetch)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    fixture.shell.Hold();
    ImageAsyncBinding blocker;
    blocker.RequestLoad(ThumbRequest(kCloud + L"0.jpg"), kCloud + L"0.jpg", ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));
    ImageAsyncBinding p1, v1, p2, v2;
    p1.RequestLoad(ThumbRequest(kCloud + L"p1.jpg"), kCloud + L"p1.jpg", ImageCore::RequestPriority::Prefetch);
    v1.RequestLoad(ThumbRequest(kCloud + L"v1.jpg"), kCloud + L"v1.jpg", ImageCore::RequestPriority::Visible);
    p2.RequestLoad(ThumbRequest(kCloud + L"p2.jpg"), kCloud + L"p2.jpg", ImageCore::RequestPriority::Prefetch);
    v2.RequestLoad(ThumbRequest(kCloud + L"v2.jpg"), kCloud + L"v2.jpg", ImageCore::RequestPriority::Prefetch);
    v2.SetPriority(ImageCore::RequestPriority::Visible);   // scrolled into view
    fixture.shell.Release();
    FIC2_REQUIRE(fixture.shell.WaitForCalls(5));
    const auto calls = fixture.shell.Calls();
    FIC2_CHECK(calls[1].path == kCloud + L"v1.jpg");
    FIC2_CHECK(calls[2].path == kCloud + L"v2.jpg");
    FIC2_CHECK(calls[3].path == kCloud + L"p1.jpg");
    FIC2_CHECK(calls[4].path == kCloud + L"p2.jpg");
}

// Navigating away: a tile rebound from an online-only file to a file of the
// new folder cancels its shell work, and the old file's late shell result
// never reaches the tile; queued shell work of recycled tiles is dropped.
FIC2_TEST(CloudThumbnail_NoStaleThumbnailReachesANewFolder)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::Canvas canvas;
    FIC2_REQUIRE(canvas.Create(64, 64));
    RouterFixture fixture(FastOptions(1));
    const std::wstring oldFile = kCloud + L"old.jpg";
    const std::wstring newFile = kLocal + L"new.png";
    fixture.shell.respond = [](const std::wstring&, uint32_t, uint32_t) { return FakeShellSource::Ready(100, 75); };

    fixture.shell.Hold();
    ImageBrowserThumbImage tile;
    tile.SetThumbnailSize({ 128.0f, 128.0f });
    FIC2_CHECK(SUCCEEDED(tile.SetSourceFile(oldFile)));
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));

    // More tiles of the old folder wait behind it.
    std::vector<std::unique_ptr<ImageBrowserThumbImage>> others;
    for (int i = 0; i < 5; ++i)
    {
        others.push_back(std::make_unique<ImageBrowserThumbImage>());
        others.back()->SetThumbnailSize({ 128.0f, 128.0f });
        FIC2_CHECK(SUCCEEDED(others.back()->SetSourceFile(kCloud + L"o" + std::to_wstring(i) + L".jpg")));
    }
    FIC2_CHECK(fixture.router->OutstandingShellRequests() == 6);

    // The new folder: every tile is recycled, this one rebinds to a local file.
    for (auto& other : others)
    {
        other->Recycle();
    }
    tile.Recycle();
    FIC2_CHECK(SUCCEEDED(tile.SetSourceFile(newFile)));
    FIC2_CHECK(fixture.Idle());
    FIC2_REQUIRE(fixture.inner.RequestCount() == 1);
    fixture.inner.Complete(fixture.inner.Requests()[0].handle, S_OK, ThumbTestSupport::MakeBgraImage(40, 30));

    fixture.shell.Release();   // the old file's shell call returns late
    std::this_thread::sleep_for(50ms);
    for (int frame = 0; frame < 3; ++frame)
    {
        FIC2_REQUIRE(SUCCEEDED(canvas.Render(tile)));
    }
    FIC2_CHECK(tile.GetBitmapSize().width == 40.0f && tile.GetBitmapSize().height == 30.0f);
    FIC2_CHECK(fixture.shell.Calls().size() == 1);   // the recycled tiles' work never ran
    FIC2_CHECK(fixture.router->GetStats().ready == 0);
    FIC2_CHECK(fixture.router->GetStats().canceled == 6);
}

// Shutdown drops queued and running requests without a completion; later
// requests pass through to the byte path.
FIC2_TEST(CloudThumbnail_ShutdownDropsRequestsAndPassesLaterOnesThrough)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    int completions = 0;
    fixture.shell.Hold();
    for (int i = 0; i < 3; ++i)
    {
        (void)fixture.router->RequestDecoded(ThumbRequest(kCloud + std::to_wstring(i) + L".jpg"),
            [&completions](HRESULT, ImageCore::DecodedImage) { ++completions; }, ImageCore::RequestPriority::Visible);
    }
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));
    std::thread release([&] { std::this_thread::sleep_for(30ms); fixture.shell.Release(); });
    FIC2_CHECK(fixture.router->Shutdown(5s));
    release.join();
    FIC2_CHECK(completions == 0);
    FIC2_CHECK(fixture.shell.Calls().size() == 1);
    FIC2_CHECK(fixture.Idle());

    const ImageCore::ImageHandle handle = fixture.router->RequestDecoded(ThumbRequest(kCloud + L"late.jpg"),
        [](HRESULT, ImageCore::DecodedImage) {}, ImageCore::RequestPriority::Visible);
    FIC2_CHECK((handle & CloudThumbnailRouter::kShellHandleBit) == 0);
    FIC2_CHECK(fixture.inner.RequestCount() == 1);
}

// The HBITMAP conversion: top-down and bottom-up DIB sections come out top-down
// BGRA8; no alpha channel reads as opaque; the shell's premultiplied alpha
// becomes straight alpha; a 24 bpp bitmap is opaque.
FIC2_TEST(CloudThumbnail_ConvertsShellBitmapsToStraightTopDownBgra)
{
    const auto pattern = [](int x, int y) -> std::array<uint8_t, 4>
    {
        return { static_cast<uint8_t>(10 + x), static_cast<uint8_t>(20 + y), static_cast<uint8_t>(30 + x + y), 0 };
    };
    for (const bool topDown : { true, false })
    {
        HBITMAP bitmap = MakeDib(3, 2, topDown, 32, pattern);
        ImageCore::DecodedImage image {};
        FIC2_REQUIRE(SUCCEEDED(ShellBitmapToDecodedImage(bitmap, image)));
        DeleteObject(bitmap);
        FIC2_CHECK(image.width == 3 && image.height == 2 && image.rowPitchBytes == 12);
        FIC2_CHECK(image.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
        FIC2_CHECK(image.alphaEncoding == ImageCore::AlphaEncoding::Opaque);
        FIC2_REQUIRE(image.blocks && image.blocks->size() == 24);
        for (uint32_t y = 0; y < 2; ++y)
        {
            for (uint32_t x = 0; x < 3; ++x)
            {
                const std::array<uint8_t, 4> expected = pattern(static_cast<int>(x), static_cast<int>(y));
                const std::array<uint8_t, 4> actual = PixelOf(image, x, y);
                FIC2_CHECK(actual[0] == expected[0] && actual[1] == expected[1] && actual[2] == expected[2]);
                FIC2_CHECK(actual[3] == 0xFF);
            }
        }
    }

    // Premultiplied: (64, 32, 0, 128) is (128, 64, 0) straight; a transparent
    // pixel stays black; an opaque one is unchanged.
    HBITMAP alpha = MakeDib(3, 1, true, 32, [](int x, int) -> std::array<uint8_t, 4>
    {
        if (x == 0)
        {
            return { 64, 32, 0, 128 };
        }
        return x == 1 ? std::array<uint8_t, 4> { 0, 0, 0, 0 } : std::array<uint8_t, 4> { 1, 2, 3, 255 };
    });
    ImageCore::DecodedImage straight {};
    FIC2_REQUIRE(SUCCEEDED(ShellBitmapToDecodedImage(alpha, straight)));
    DeleteObject(alpha);
    FIC2_CHECK(straight.alphaEncoding == ImageCore::AlphaEncoding::Straight);
    const auto p0 = PixelOf(straight, 0, 0);
    FIC2_CHECK(p0[0] == 128 && p0[1] == 64 && p0[2] == 0 && p0[3] == 128);
    const auto p1 = PixelOf(straight, 1, 0);
    FIC2_CHECK(p1[0] == 0 && p1[1] == 0 && p1[2] == 0 && p1[3] == 0);
    const auto p2 = PixelOf(straight, 2, 0);
    FIC2_CHECK(p2[0] == 1 && p2[1] == 2 && p2[2] == 3 && p2[3] == 255);

    // 24 bpp, bottom-up.
    HBITMAP rgb = MakeDib(2, 2, false, 24, [](int x, int y) -> std::array<uint8_t, 4>
    {
        return { static_cast<uint8_t>(x * 100), static_cast<uint8_t>(y * 100), 7, 0 };
    });
    ImageCore::DecodedImage opaque {};
    FIC2_REQUIRE(SUCCEEDED(ShellBitmapToDecodedImage(rgb, opaque)));
    DeleteObject(rgb);
    FIC2_CHECK(opaque.alphaEncoding == ImageCore::AlphaEncoding::Opaque);
    const auto q = PixelOf(opaque, 1, 1);
    FIC2_CHECK(q[0] == 100 && q[1] == 100 && q[2] == 7 && q[3] == 255);

    ImageCore::DecodedImage none {};
    FIC2_CHECK(FAILED(ShellBitmapToDecodedImage(nullptr, none)));
}

namespace
{
    const Floar::VirtualPath kCloudFolder(std::filesystem::path(L"C:\\Cloud\\Strip"));

    bool IsJpeg(const Floar::VirtualPath& path)
    {
        return path.GetExtension() == L".jpg";
    }

    std::wstring StableName(const wchar_t* prefix, const Floar::VirtualPath& path)
    {
        return std::wstring(prefix) + L"|" + ThumbItemModel::LookupKeyFor(path);
    }

    // A strip of online-only images with the browser's wiring (aspects and
    // source-limited heights go to the model, row heights to the tiles), whose
    // thumbnails come from the router.
    struct CloudStrip
    {
        ThumbTestSupport::ComScope com {};
        ThumbTestSupport::Canvas canvas {};
        RouterFixture fixture { FastOptions(2) };
        std::shared_ptr<ThumbPayloadCache> payloads { std::make_shared<ThumbPayloadCache>() };
        ThumbItemModel model {};
        ThumbTileSet tiles {};
        std::shared_ptr<ThumbStripPanel> panel { std::make_shared<ThumbStripPanel>() };
        ImageBrowserThumbStripController controller {};
        float rowHeight { 128.0f };

        explicit CloudStrip(size_t count)
        {
            FIC2_REQUIRE(canvas.Create(64, 64));
            std::vector<Floar::VirtualFileEntry> listing;
            std::vector<ImageSourceIdentity> identities;
            for (size_t i = 0; i < count; ++i)
            {
                listing.emplace_back(Floar::VirtualPath(kCloudFolder.hostPath / (L"p" + std::to_wstring(i) + L".jpg")),
                    false, 0, 0);
                ImageSourceIdentity identity {};
                identity.size = 1000 + i;
                identity.writeTime = 1;
                identity.known = true;
                identities.push_back(identity);
            }
            model.Reset(kCloudFolder, false, &IsJpeg);
            (void)model.Merge(listing, identities);
            panel->SetTiles(&tiles);
        }

        ~CloudStrip()
        {
            fixture.shell.Release();
            tiles.Clear();
            panel->ClearChildren();
        }

        void Materialize(ThumbIndexRange range)
        {
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = panel;
            context.tiles = &tiles;
            context.thumbW = rowHeight;
            context.thumbH = rowHeight;
            context.model = &model;
            context.range = range;
            context.visible = range;
            context.payloads = payloads;
            context.makeStableName = &StableName;
            context.onImageAspect = [this](const std::wstring& key, float aspect) { (void)model.SetAspect(key, aspect); };
            context.onImageSourceLimited = [this](const std::wstring& key, uint32_t height)
            {
                (void)model.SetSourceLimited(key, height);
            };
            controller.MaterializeTiles(context);
        }

        void SetRowHeight(float height)
        {
            rowHeight = height;
            for (const ThumbTile& tile : tiles.Tiles())
            {
                tile.imageTile->SetFixedHeight(height);
            }
        }

        // Shell results arrive, then frames show them and evaluate upgrades.
        void Settle(int frames = 4)
        {
            for (int i = 0; i < frames; ++i)
            {
                FIC2_REQUIRE(WaitUntil([&] { return fixture.Idle(); }));
                ImageBrowserThumbStripController::AdmitCompletedPayloads(tiles);
                for (const ThumbTile& tile : tiles.Tiles())
                {
                    FIC2_REQUIRE(SUCCEEDED(canvas.Render(*tile.imageTile)));
                }
            }
        }

        D2D1_SIZE_F BitmapOf(size_t index) const
        {
            const ThumbTile* tile = tiles.Find(index);
            FIC2_REQUIRE(tile != nullptr);
            return tile->image->GetBitmapSize();
        }
    };

    // A provider whose thumbnails are at most `width` x `height`: the shell
    // fits that into the box, never larger.
    ShellThumbnailResult ProviderFit(uint32_t width, uint32_t height, uint32_t boxW, uint32_t boxH)
    {
        const float scale = (std::min)(1.0f, (std::min)(static_cast<float>(boxW) / width, static_cast<float>(boxH) / height));
        return FakeShellSource::Ready(
            static_cast<uint32_t>(std::lround(width * scale)), static_cast<uint32_t>(std::lround(height * scale)));
    }
}

// Size: a shell thumbnail smaller than the row asks for is published at its
// actual size, recorded as source-limited, and not requested again, neither
// when the row grows nor when the item is bound again; the byte path is
// never used to upgrade it.
FIC2_TEST(CloudThumbnail_ASmallerShellThumbnailIsNotRequestedAgain)
{
    CloudStrip strip(2);
    // The provider holds 4:3 thumbnails of at most 160 x 120.
    strip.fixture.shell.respond = [](const std::wstring&, uint32_t w, uint32_t h) { return ProviderFit(160, 120, w, h); };

    strip.rowHeight = 96.0f;
    strip.Materialize({ 0, 2 });
    strip.Settle();
    // The square first box gives 96 x 72; one upgrade with the aspect gives 128 x 96.
    FIC2_CHECK(strip.BitmapOf(0).height == 96.0f);
    const size_t afterFirst = strip.fixture.shell.Calls().size();
    FIC2_CHECK(afterFirst == 4);   // two per item

    // The row grows past what the provider has: one more shell request per
    // item, which comes back source-limited at 160 x 120.
    strip.SetRowHeight(192.0f);
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 120.0f);
    const size_t afterGrow = strip.fixture.shell.Calls().size();
    FIC2_CHECK(afterGrow == afterFirst + 2);

    // Larger rows, more frames, and binding the items again: nothing more.
    strip.SetRowHeight(256.0f);
    strip.Settle();
    strip.SetRowHeight(384.0f);
    strip.Settle(6);
    strip.tiles.Clear();
    strip.panel->ClearChildren();
    strip.Materialize({ 0, 2 });
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 120.0f);
    FIC2_CHECK(strip.fixture.shell.Calls().size() == afterGrow);
    FIC2_CHECK(strip.fixture.inner.RequestCount() == 0);
}

namespace
{
    // A recording inner service whose RequestDecoded and SetPriority can be
    // made to wait, for races around a request's fallback.
    class GatedDecodeService final : public ThumbTestSupport::RecordingDecodeService
    {
    public:
        // RequestDecoded signals that it is dispatching and waits for
        // ReleaseDispatch().
        bool holdDispatch { false };
        // The first SetPriority to Prefetch signals that it is reconciling and
        // waits up to `reconcileWait` for a later SetPriority to arrive.
        bool holdPrefetchUpdate { false };
        std::chrono::milliseconds reconcileWait { 300 };

        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback,
            ImageCore::RequestPriority priority) override
        {
            if (holdDispatch)
            {
                std::unique_lock<std::mutex> lock(m_gate);
                m_dispatching = true;
                m_changed.notify_all();
                m_changed.wait(lock, [this] { return m_dispatchReleased; });
            }
            return RecordingDecodeService::RequestDecoded(request, std::move(callback), priority);
        }

        void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) override
        {
            {
                std::unique_lock<std::mutex> lock(m_gate);
                ++m_updates;
                m_changed.notify_all();
                if (holdPrefetchUpdate && !m_reconciling && priority == ImageCore::RequestPriority::Prefetch)
                {
                    m_reconciling = true;
                    const size_t updatesAtHold = m_updates;
                    m_changed.notify_all();
                    m_changed.wait_for(lock, reconcileWait, [&] { return m_updates > updatesAtHold; });
                }
            }
            RecordingDecodeService::SetPriority(handle, priority);
        }

        bool WaitDispatching() { return WaitFor([this] { return m_dispatching; }); }
        bool WaitReconciling() { return WaitFor([this] { return m_reconciling; }); }

        void ReleaseDispatch()
        {
            std::lock_guard<std::mutex> lock(m_gate);
            m_dispatchReleased = true;
            m_changed.notify_all();
        }

    private:
        template <typename Predicate>
        bool WaitFor(Predicate predicate)
        {
            std::unique_lock<std::mutex> lock(m_gate);
            return m_changed.wait_for(lock, 5s, predicate);
        }

        std::mutex m_gate;
        std::condition_variable m_changed;
        bool m_dispatching { false };
        bool m_dispatchReleased { false };
        bool m_reconciling { false };
        size_t m_updates { 0 };
    };

    ShellThumbnailResult FailedShell(const std::wstring&, uint32_t, uint32_t)
    {
        return FakeShellSource::Status(ShellThumbnailStatus::Failed, E_FAIL);
    }
}

// A request canceled after its shell call failed but before its fallback is
// admitted never reaches the byte path, so it cannot start a download.
FIC2_TEST(CloudThumbnail_CancelBeforeFallbackAdmissionStartsNoRead)
{
    ThumbTestSupport::ComScope com;
    ThumbTestSupport::RecordingDecodeService inner;
    FakeQueries queries;
    FakeShellSource shell;
    shell.respond = FailedShell;

    std::mutex mutex;
    std::condition_variable changed;
    bool atFallback = false;
    bool proceed = false;
    CloudThumbnailRouter::Options options = FastOptions(1);
    options.beforeFallbackForTesting = [&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        atFallback = true;
        changed.notify_all();
        changed.wait(lock, [&] { return proceed; });
    };
    CloudThumbnailRouter router(inner, shell, &queries, options);

    bool called = false;
    const ImageCore::ImageHandle handle = router.RequestDecoded(ThumbRequest(kCloud + L"a.dds"),
        [&called](HRESULT, ImageCore::DecodedImage) { called = true; }, ImageCore::RequestPriority::Visible);
    {
        std::unique_lock<std::mutex> lock(mutex);
        FIC2_REQUIRE(changed.wait_for(lock, 5s, [&] { return atFallback; }));
    }
    router.Cancel(handle);   // returns at once: nothing was admitted yet
    {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
        changed.notify_all();
    }
    FIC2_CHECK(router.Shutdown(5s));
    FIC2_CHECK(inner.RequestCount() == 0);
    FIC2_CHECK(!called);
}

// Priority changes that overlap a fallback's admission and its priority
// reconciliation: the inner request ends at the last priority set.
FIC2_TEST(CloudThumbnail_TheLatestPriorityWinsAcrossAFallback)
{
    ThumbTestSupport::ComScope com;
    GatedDecodeService inner;
    inner.holdDispatch = true;
    inner.holdPrefetchUpdate = true;
    FakeQueries queries;
    FakeShellSource shell;
    shell.respond = FailedShell;
    CloudThumbnailRouter router(inner, shell, &queries, FastOptions(1));

    const ImageCore::ImageHandle handle = router.RequestDecoded(ThumbRequest(kCloud + L"a.dds"),
        [](HRESULT, ImageCore::DecodedImage) {}, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(inner.WaitDispatching());

    // Demoted while the fallback is dispatched at Visible...
    std::thread demote([&] { router.SetPriority(handle, ImageCore::RequestPriority::Prefetch); });
    std::this_thread::sleep_for(50ms);
    inner.ReleaseDispatch();
    // ...and promoted again while the demotion is being forwarded.
    FIC2_REQUIRE(inner.WaitReconciling());
    std::thread promote([&] { router.SetPriority(handle, ImageCore::RequestPriority::Visible); });
    demote.join();
    promote.join();

    const auto requests = inner.Requests();
    FIC2_REQUIRE(requests.size() == 1);
    FIC2_CHECK(requests[0].priority == ImageCore::RequestPriority::Visible);
    FIC2_CHECK(router.Shutdown(5s));
}

// Cancel and SetPriority (called on the UI thread) never wait for a fallback's
// admission, which may block in the inner service; a cancel made during it
// cancels the inner request once the admission returns.
FIC2_TEST(CloudThumbnail_CancelAndSetPriorityNeverWaitForAdmission)
{
    ThumbTestSupport::ComScope com;
    GatedDecodeService inner;
    inner.holdDispatch = true;
    FakeQueries queries;
    FakeShellSource shell;
    shell.respond = FailedShell;
    CloudThumbnailRouter router(inner, shell, &queries, FastOptions(1));

    bool called = false;
    const ImageCore::ImageHandle handle = router.RequestDecoded(ThumbRequest(kCloud + L"a.dds"),
        [&called](HRESULT, ImageCore::DecodedImage) { called = true; }, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(inner.WaitDispatching());

    std::mutex mutex;
    std::condition_variable changed;
    bool returned = false;
    std::thread caller([&]
    {
        router.SetPriority(handle, ImageCore::RequestPriority::Prefetch);
        router.Cancel(handle);
        std::lock_guard<std::mutex> lock(mutex);
        returned = true;
        changed.notify_all();
    });
    bool returnedDuringAdmission = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        returnedDuringAdmission = changed.wait_for(lock, 1s, [&] { return returned; });
    }
    inner.ReleaseDispatch();
    caller.join();
    FIC2_CHECK(returnedDuringAdmission);

    FIC2_REQUIRE(WaitUntil([&] { return inner.RequestCount() == 1 && inner.Requests()[0].canceled; }));
    FIC2_CHECK(router.Shutdown(5s));
    FIC2_CHECK(!called);
}

// Concurrent Shutdown calls: while one still waits for a worker in a shell
// call, another reports false at its own bound instead of success.
FIC2_TEST(CloudThumbnail_ConcurrentShutdownReportsRunningWorkers)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    fixture.shell.Hold();
    (void)fixture.router->RequestDecoded(ThumbRequest(kCloud + L"a.jpg"),
        [](HRESULT, ImageCore::DecodedImage) {}, ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(fixture.shell.WaitForCalls(1));

    bool first = false;
    std::thread owner([&] { first = fixture.router->Shutdown(5s); });
    std::this_thread::sleep_for(50ms);
    const bool second = fixture.router->Shutdown(20ms);
    fixture.shell.Release();
    owner.join();
    FIC2_CHECK(!second);
    FIC2_CHECK(first);
    FIC2_CHECK(fixture.router->Shutdown(0ms));   // all joined
}

namespace
{
    // A provider that completes a pending thumbnail through the calling
    // thread's apartment: the first call posts a message to a message-only
    // window of that thread and returns Pending; the thumbnail is ready once
    // the message has been dispatched.
    std::atomic<bool> g_providerCompleted { false };
    constexpr UINT kProviderCompletion = WM_APP + 59;

    LRESULT CALLBACK ProviderWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kProviderCompletion)
        {
            g_providerCompleted = true;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND ProviderWindowOfThisThread()
    {
        static const ATOM atom = []
        {
            WNDCLASSW windowClass {};
            windowClass.lpfnWndProc = ProviderWindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"FICture2TestsShellProvider";
            return RegisterClassW(&windowClass);
        }();
        thread_local const HWND window = atom == 0 ? nullptr
            : CreateWindowExW(0, L"FICture2TestsShellProvider", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                GetModuleHandleW(nullptr), nullptr);
        return window;
    }

    // Holds a worker thread's exit (its thread-local cleanup) until opened.
    struct ExitGate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open { false };

        void Open()
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
            changed.notify_all();
        }
    };
    ExitGate g_exitGate;

    struct BlockedThreadCleanup
    {
        bool armed { false };

        ~BlockedThreadCleanup()
        {
            if (armed)
            {
                std::unique_lock<std::mutex> lock(g_exitGate.mutex);
                g_exitGate.changed.wait_for(lock, 10s, [] { return g_exitGate.open; });
            }
        }
    };
    thread_local BlockedThreadCleanup t_blockedCleanup;
}

// The workers dispatch their apartment's messages, so a provider that
// completes pending thumbnails through them finishes without a fallback.
FIC2_TEST(CloudThumbnail_WorkersPumpTheirApartmentWhilePending)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    g_providerCompleted = false;
    auto posted = std::make_shared<std::atomic<bool>>(false);
    fixture.shell.respond = [posted](const std::wstring&, uint32_t, uint32_t)
    {
        if (g_providerCompleted)
        {
            return FakeShellSource::Ready(64, 48);
        }
        if (!posted->exchange(true))
        {
            PostMessageW(ProviderWindowOfThisThread(), kProviderCompletion, 0, 0);
        }
        return FakeShellSource::Status(ShellThumbnailStatus::Pending, static_cast<HRESULT>(0x8004B205));
    };

    ImageAsyncBinding binding;
    binding.RequestLoad(ThumbRequest(kCloud + L"a.jpg"), kCloud + L"a.jpg", ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(WaitUntil([&] { return fixture.Idle(); }));
    FIC2_CHECK(posted->load());
    FIC2_CHECK(binding.CurrentPhase() == ImageAsyncBinding::Phase::Ready);
    FIC2_CHECK(fixture.router->GetStats().ready == 1);
    FIC2_CHECK(fixture.inner.RequestCount() == 0);   // no downloading fallback
}

// Shutdown is bounded by the workers' thread exit, not only by their loop: a
// worker whose thread-local cleanup blocks makes Shutdown return false at its
// bound; the worker is kept, and a later Shutdown joins it.
FIC2_TEST(CloudThumbnail_ShutdownIsBoundedByWorkerThreadExit)
{
    ThumbTestSupport::ComScope com;
    RouterFixture fixture(FastOptions(1));
    fixture.shell.respond = [](const std::wstring&, uint32_t w, uint32_t h)
    {
        t_blockedCleanup.armed = true;
        return FakeShellSource::Ready(w, h);
    };
    ImageAsyncBinding binding;
    binding.RequestLoad(ThumbRequest(kCloud + L"a.jpg"), kCloud + L"a.jpg", ImageCore::RequestPriority::Visible);
    FIC2_REQUIRE(WaitForPhase(binding, ImageAsyncBinding::Phase::Ready));

    const auto start = std::chrono::steady_clock::now();
    const bool stopped = fixture.router->Shutdown(20ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    g_exitGate.Open();
    FIC2_CHECK(!stopped);
    FIC2_CHECK(elapsed < 2s);
    FIC2_CHECK(fixture.router->Shutdown(5s));
}
