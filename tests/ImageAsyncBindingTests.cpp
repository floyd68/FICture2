#include "TestHarness.h"

#include "CommonUtil.h"
#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbImage.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ImageAsyncBinding tests.
//
// Ordering-sensitive cases (stale completions, cancellation, lifetime) use a
// fake decode service that retains callbacks, so the test decides exactly when
// and in which order completions arrive. A few end-to-end cases run against the
// real ImageCore loader with tiny BMP files; they wait for the complete state
// transition with a bounded timeout and never rely on elapsed time alone.

namespace
{
    constexpr auto kWaitTimeout = std::chrono::milliseconds(10000);

    bool WaitUntil(const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate();
    }

    ImageCore::DecodedImage MakeBgraImage(uint32_t width, uint32_t height)
    {
        ImageCore::DecodedImage image {};
        image.width = width;
        image.height = height;
        image.rowPitchBytes = width * 4u;
        image.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        image.blocks = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(width) * height * 4u, static_cast<uint8_t>(0x80));
        image.blockBytes = static_cast<uint32_t>(image.blocks->size());
        image.sourceMipLevels = 1;
        return image;
    }

    // ---------------------------------------------------------------------
    // Fake decode service: records requests/cancels, delivers on demand.
    // ---------------------------------------------------------------------

    class FakeDecodeService : public IImageDecodeService
    {
    public:
        struct Request
        {
            ImageCore::ImageHandle handle { 0 };
            ImageCore::ImageRequest request {};
            ImageCore::DecodedImageLoadCallback callback {};
            ImageCore::RequestPriority priority { ImageCore::RequestPriority::Visible };
        };

        // Next RequestDecoded returns 0 without keeping the callback (ImageCore
        // rejecting a request, e.g. after shutdown).
        bool rejectNext = false;
        // Next RequestDecoded completes successfully before it returns (a worker
        // finishing before the binding stores the handle).
        bool completeBeforeReturn = false;
        // Runs inside the next RequestDecoded, before it returns (UI work that
        // reaches the binding while the request is being dispatched).
        std::function<void()> duringDispatch {};

        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback,
            ImageCore::RequestPriority priority) override
        {
            ImageCore::DecodedImageLoadCallback immediate;
            ImageCore::ImageHandle handle = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (rejectNext)
                {
                    rejectNext = false;
                    return 0;
                }
                handle = ++m_nextHandle;
                requests.push_back({ handle, request, callback, priority });
                if (completeBeforeReturn)
                {
                    completeBeforeReturn = false;
                    immediate = callback;
                }
            }
            if (immediate)
            {
                immediate(S_OK, MakeBgraImage(2, 2));
            }
            if (auto hook = std::exchange(duringDispatch, {}))
            {
                hook();
            }
            return handle;
        }

        void Cancel(ImageCore::ImageHandle handle) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancels.push_back(handle);
        }

        void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            priorityChanges.push_back({ handle, priority });
        }

        std::vector<std::pair<ImageCore::ImageHandle, ImageCore::RequestPriority>> priorityChanges;

        // Delivers the completion of request `index` (as a worker thread would).
        // Safe to call from any thread.
        void Complete(size_t index, HRESULT hr, ImageCore::DecodedImage image = {})
        {
            ImageCore::DecodedImageLoadCallback callback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                callback = requests.at(index).callback;
            }
            callback(hr, std::move(image));
        }

        bool WasCanceled(ImageCore::ImageHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto canceled : cancels)
            {
                if (canceled == handle)
                {
                    return true;
                }
            }
            return false;
        }

        size_t RequestCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return requests.size();
        }

        std::vector<Request> requests;
        std::vector<ImageCore::ImageHandle> cancels;

    private:
        mutable std::mutex m_mutex;
        ImageCore::ImageHandle m_nextHandle { 0 };
    };

    struct ScopedFakeDecodeService
    {
        FakeDecodeService fake;

        ScopedFakeDecodeService()
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(&fake);
        }

        ~ScopedFakeDecodeService()
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }
    };

    const std::wstring& FakePathA()
    {
        static const std::wstring path = CommonUtil::NormalizePath(L"C:\\FICture2Tests\\a.dds");
        return path;
    }

    const std::wstring& FakePathB()
    {
        static const std::wstring path = CommonUtil::NormalizePath(L"C:\\FICture2Tests\\b.dds");
        return path;
    }

    // ---------------------------------------------------------------------
    // Real-loader fixture: tiny BMP files in a temporary directory.
    // ---------------------------------------------------------------------

    // Writes a top-down 32bpp BI_RGB bitmap of the given size.
    void WriteBmp(const std::filesystem::path& path, uint32_t width, uint32_t height)
    {
        const uint32_t pixelBytes = width * height * 4u;

        BITMAPFILEHEADER file {};
        file.bfType = 0x4D42; // 'BM'
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + pixelBytes;

        BITMAPINFOHEADER info {};
        info.biSize = sizeof(BITMAPINFOHEADER);
        info.biWidth = static_cast<LONG>(width);
        info.biHeight = -static_cast<LONG>(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = pixelBytes;

        std::vector<uint8_t> pixels(pixelBytes, 0x80);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }

    struct Fixture
    {
        std::filesystem::path dir;
        std::wstring smallBmp;   // 4 x 2
        std::wstring otherBmp;   // 6 x 3
        std::wstring missingBmp;

        Fixture()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2Tests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            WriteBmp(dir / L"small.bmp", 4, 2);
            WriteBmp(dir / L"other.bmp", 6, 3);

            smallBmp = CommonUtil::NormalizePath((dir / L"small.bmp").wstring());
            otherBmp = CommonUtil::NormalizePath((dir / L"other.bmp").wstring());
            missingBmp = CommonUtil::NormalizePath((dir / L"missing.bmp").wstring());

            ImageCore::RegisterBuiltInDecoders();
            TestHarness::RegisterTeardown([this]()
            {
                ImageAsyncBindingRegistry::Instance().ShutdownPrepare();
                ImageCore::ImageLoader::Instance().Shutdown();
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            });
        }
    };

    Fixture& Files()
    {
        static Fixture fixture;
        return fixture;
    }

    ImageCore::ImageRequest MakeRequest(const std::wstring& path)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::FullResolution);
        request.allowGpuCompressedDDS = false;
        return request;
    }

    void Load(ImageAsyncBinding& binding, const std::wstring& path)
    {
        binding.RequestLoad(MakeRequest(path), path);
    }

    ImageAsyncBinding::Payload Apply(ImageAsyncBinding& binding, bool& applied)
    {
        ImageAsyncBinding::Payload result {};
        applied = binding.PollAndApply([&](ImageAsyncBinding::Payload&& payload)
        {
            result = std::move(payload);
        });
        return result;
    }
}

// -------------------------------------------------------------------------
// Controlled completion delivery (fake decode service)
// -------------------------------------------------------------------------

FIC2_TEST(ImageAsyncBinding_PriorityReachesTheDecodeService)
{
    using ImageCore::RequestPriority;
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    // Default: the purpose's priority (MakeRequest is full resolution: MainImage).
    binding.RequestLoad(MakeRequest(FakePathA()), FakePathA());
    FIC2_REQUIRE(service.fake.RequestCount() == 1);
    FIC2_CHECK(service.fake.requests[0].priority == RequestPriority::MainImage);

    // Explicit priority, then promotion of the request in flight.
    binding.RequestLoad(MakeRequest(FakePathB()), FakePathB(), RequestPriority::Prefetch);
    FIC2_REQUIRE(service.fake.RequestCount() == 2);
    FIC2_CHECK(service.fake.requests[1].priority == RequestPriority::Prefetch);
    const ImageCore::ImageHandle handle = service.fake.requests[1].handle;
    binding.SetPriority(RequestPriority::Visible);
    binding.SetPriority(RequestPriority::Visible);   // unchanged: not forwarded
    FIC2_REQUIRE(service.fake.priorityChanges.size() == 1);
    FIC2_CHECK(service.fake.priorityChanges[0].first == handle);
    FIC2_CHECK(service.fake.priorityChanges[0].second == RequestPriority::Visible);

    // Finished requests are not re-prioritized.
    service.fake.Complete(1, S_OK, MakeBgraImage(2, 2));
    binding.SetPriority(RequestPriority::Prefetch);
    FIC2_CHECK(service.fake.priorityChanges.size() == 1);
}

FIC2_TEST(ImageAsyncBinding_PromotionDuringDispatchIsApplied)
{
    using ImageCore::RequestPriority;
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    // The tile scrolls into view while RequestDecoded is still returning: the
    // handle is not stored yet, so the promotion is applied once it is.
    service.fake.duringDispatch = [&] { binding.SetPriority(RequestPriority::Visible); };
    binding.RequestLoad(MakeRequest(FakePathA()), FakePathA(), RequestPriority::Prefetch);
    FIC2_REQUIRE(service.fake.RequestCount() == 1);
    FIC2_REQUIRE(service.fake.priorityChanges.size() == 1);
    FIC2_CHECK(service.fake.priorityChanges[0].first == service.fake.requests[0].handle);
    FIC2_CHECK(service.fake.priorityChanges[0].second == RequestPriority::Visible);
}

FIC2_TEST(ImageAsyncBinding_ThumbnailLoadPriorityFollowsTheTile)
{
    using ImageCore::RequestPriority;
    ScopedFakeDecodeService service;
    {
        auto thumb = std::make_shared<ImageBrowserThumbImage>(L"thumb");
        // Tiles start as prefetch work until the strip sees them on screen.
        FIC2_CHECK(thumb->LoadPriority() == RequestPriority::Prefetch);
        (void)thumb->SetSourceFile(FakePathA());
        FIC2_REQUIRE(service.fake.RequestCount() == 1);
        FIC2_CHECK(service.fake.requests[0].priority == RequestPriority::Prefetch);

        thumb->SetLoadPriority(RequestPriority::Visible);
        FIC2_REQUIRE(service.fake.priorityChanges.size() == 1);
        FIC2_CHECK(service.fake.priorityChanges[0].second == RequestPriority::Visible);

        // The next file of a reused tile is requested at its current priority.
        (void)thumb->SetSourceFile(FakePathB());
        FIC2_REQUIRE(service.fake.RequestCount() == 2);
        FIC2_CHECK(service.fake.requests[1].priority == RequestPriority::Visible);
    }
}

FIC2_TEST(ImageAsyncBinding_NewerRequestCancelsSupersededHandle)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    Load(binding, FakePathB());

    FIC2_REQUIRE(service.fake.requests.size() == 2);
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[0].handle));
    FIC2_CHECK(!service.fake.WasCanceled(service.fake.requests[1].handle));
    FIC2_CHECK(binding.IsLoading());
}

FIC2_TEST(ImageAsyncBinding_StaleSuccessAfterNewerStagedIsIgnored)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    Load(binding, FakePathB());
    FIC2_REQUIRE(service.fake.requests.size() == 2);

    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    FIC2_REQUIRE(binding.HasPendingCpuBgra8For(FakePathB()));

    // The superseded request completes after the newer one was staged.
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathB()));
    FIC2_CHECK(!binding.HasPendingCpuBgra8For(FakePathA()));

    bool applied = false;
    const auto payload = Apply(binding, applied);
    FIC2_CHECK(applied);
    FIC2_CHECK(payload.sourcePath == FakePathB());
    FIC2_CHECK(payload.width == 8);
    FIC2_CHECK(payload.height == 6);
    FIC2_CHECK(!binding.IsLoading());
}

FIC2_TEST(ImageAsyncBinding_StaleSuccessAfterNewerAppliedIsIgnored)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    Load(binding, FakePathB());
    FIC2_REQUIRE(service.fake.requests.size() == 2);

    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    bool applied = false;
    (void)Apply(binding, applied);
    FIC2_REQUIRE(applied);

    // A late completion of the superseded request must not re-stage a payload.
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
    FIC2_CHECK(!binding.IsLoading());
}

FIC2_TEST(ImageAsyncBinding_StaleFailureDoesNotAffectNewerRequest)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    Load(binding, FakePathB());
    FIC2_REQUIRE(service.fake.requests.size() == 2);

    service.fake.Complete(0, E_FAIL);
    FIC2_CHECK(binding.IsLoading());
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));
    FIC2_CHECK(!binding.IsFailedFor(FakePathB()));

    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathB()));
}

FIC2_TEST(ImageAsyncBinding_FailureCompletionSettlesState)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    FIC2_REQUIRE(service.fake.requests.size() == 1);

    service.fake.Complete(0, E_FAIL);
    FIC2_CHECK(binding.IsFailedFor(FakePathA()));
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
}

FIC2_TEST(ImageAsyncBinding_CancelDropsLaterCompletion)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    FIC2_REQUIRE(service.fake.requests.size() == 1);

    binding.Cancel();
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[0].handle));
    FIC2_CHECK(!binding.IsLoading());

    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
    FIC2_CHECK(!binding.IsLoading());

    service.fake.Complete(0, E_FAIL);
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));
}

FIC2_TEST(ImageAsyncBinding_RegistryInvalidateAllCancelsLiveBindings)
{
    ScopedFakeDecodeService service;
    ImageAsyncBinding first;
    ImageAsyncBinding second;

    Load(first, FakePathA());
    Load(second, FakePathB());
    FIC2_REQUIRE(service.fake.requests.size() == 2);

    ImageAsyncBindingRegistry::Instance().InvalidateAll();
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[0].handle));
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[1].handle));
    FIC2_CHECK(!first.IsLoading());
    FIC2_CHECK(!second.IsLoading());

    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    FIC2_CHECK(first.TakePending().blocks == nullptr);
    FIC2_CHECK(second.TakePending().blocks == nullptr);
}

// The registry is handle-based: entries are found by handle, not by a scan of
// the live bindings, so registration and removal do not grow with their count.
FIC2_TEST(ImageAsyncBinding_RegistryUnregistersByHandleInAnyOrder)
{
    ImageAsyncBindingRegistry registry;
    std::vector<std::shared_ptr<ImageAsyncBindingState>> states;
    std::vector<ImageAsyncBindingRegistry::Handle> handles;
    for (int i = 0; i < 6; ++i)
    {
        states.push_back(std::make_shared<ImageAsyncBindingState>());
        handles.push_back(registry.Register(states.back()));
        FIC2_CHECK(handles.back() != 0);
    }
    FIC2_CHECK(registry.Register(nullptr) == 0);
    FIC2_CHECK(registry.Count() == 6);
    for (size_t i = 0; i < handles.size(); ++i)
    {
        for (size_t j = i + 1; j < handles.size(); ++j)
        {
            FIC2_CHECK(handles[i] != handles[j]);
        }
    }

    // Arbitrary order, including the middle, the ends, a repeat and an unknown
    // handle; every other entry stays registered.
    registry.Unregister(handles[3]);
    registry.Unregister(handles[0]);
    registry.Unregister(handles[5]);
    registry.Unregister(handles[3]);
    registry.Unregister(0);
    registry.Unregister(handles[5] + 1000);
    FIC2_CHECK(registry.Count() == 3);
    FIC2_CHECK(!registry.IsRegistered(handles[0]));
    FIC2_CHECK(registry.IsRegistered(handles[1]));
    FIC2_CHECK(registry.IsRegistered(handles[2]));
    FIC2_CHECK(!registry.IsRegistered(handles[3]));
    FIC2_CHECK(registry.IsRegistered(handles[4]));
    FIC2_CHECK(!registry.IsRegistered(handles[5]));

    // Re-registering after removals never reuses a live handle.
    const ImageAsyncBindingRegistry::Handle again = registry.Register(states[3]);
    FIC2_CHECK(again != handles[1] && again != handles[2] && again != handles[4]);
    FIC2_CHECK(registry.Count() == 4);
    registry.Unregister(again);
    FIC2_CHECK(registry.Count() == 3);
}

FIC2_TEST(ImageAsyncBinding_RegistryInvalidateAllReachesOnlyLiveEntries)
{
    ImageAsyncBindingRegistry registry;
    std::vector<std::shared_ptr<ImageAsyncBindingState>> states;
    std::vector<ImageAsyncBindingRegistry::Handle> handles;
    for (int i = 0; i < 5; ++i)
    {
        states.push_back(std::make_shared<ImageAsyncBindingState>());
        handles.push_back(registry.Register(states.back()));
    }
    registry.Unregister(handles[1]);
    registry.Unregister(handles[4]);

    // An entry whose binding state is gone is skipped and dropped.
    auto expired = std::make_shared<ImageAsyncBindingState>();
    (void)registry.Register(expired);
    expired.reset();
    FIC2_CHECK(registry.Count() == 4);

    auto generationOf = [](const ImageAsyncBindingState& state)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.generation;
    };

    registry.InvalidateAll();
    FIC2_CHECK(generationOf(*states[0]) == 1);
    FIC2_CHECK(generationOf(*states[1]) == 0);   // removed
    FIC2_CHECK(generationOf(*states[2]) == 1);
    FIC2_CHECK(generationOf(*states[3]) == 1);
    FIC2_CHECK(generationOf(*states[4]) == 0);   // removed
    FIC2_CHECK(registry.Count() == 3);

    // A binding registers with the global registry for its lifetime.
    const size_t before = ImageAsyncBindingRegistry::Instance().Count();
    {
        ImageAsyncBinding first;
        ImageAsyncBinding second;
        FIC2_CHECK(ImageAsyncBindingRegistry::Instance().Count() == before + 2);
    }
    FIC2_CHECK(ImageAsyncBindingRegistry::Instance().Count() == before);
}

FIC2_TEST(ImageAsyncBinding_LateCompletionAfterDestructionIsSafe)
{
    ScopedFakeDecodeService service;
    {
        ImageAsyncBinding binding;
        Load(binding, FakePathA());
    }
    FIC2_REQUIRE(service.fake.requests.size() == 1);
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[0].handle));

    // The callback only owns the shared state; delivering it after the
    // binding is gone must neither crash nor affect a new binding.
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));

    ImageAsyncBinding next;
    Load(next, FakePathB());
    FIC2_REQUIRE(service.fake.requests.size() == 2);
    FIC2_CHECK(next.TakePending().blocks == nullptr);
    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    FIC2_CHECK(next.HasPendingCpuBgra8For(FakePathB()));
}

FIC2_TEST(ImageAsyncBinding_PollWithoutPendingDoesNothing)
{
    ImageAsyncBinding binding;
    bool called = false;
    FIC2_CHECK(!binding.PollAndApply([&](ImageAsyncBinding::Payload&&) { called = true; }));
    FIC2_CHECK(!called);
    FIC2_CHECK(!binding.PollAndApply({}));
}

FIC2_TEST(ImageAsyncBinding_FailureBookkeeping)
{
    ImageAsyncBinding binding;

    binding.RecordFailure(FakePathA(), E_FAIL);
    FIC2_CHECK(binding.IsFailedFor(FakePathA()));
    FIC2_CHECK(!binding.IsFailedFor(FakePathB()));
    FIC2_CHECK(!binding.ClearFailureIfMatches(FakePathB()));
    FIC2_CHECK(binding.ClearFailureIfMatches(FakePathA(), true /*requireFailedHr*/));
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));

    // A recorded success HRESULT does not satisfy requireFailedHr.
    binding.RecordFailure(FakePathA(), S_OK);
    FIC2_CHECK(!binding.ClearFailureIfMatches(FakePathA(), true));
    FIC2_CHECK(binding.ClearFailureIfMatches(FakePathA(), false));

    binding.RecordFailure(FakePathA(), E_FAIL);
    binding.ClearFailure();
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));
}

FIC2_TEST(ImageAsyncBinding_PhasesFollowRequestLifecycle)
{
    using Phase = ImageAsyncBinding::Phase;
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;
    FIC2_CHECK(binding.CurrentPhase() == Phase::Idle);
    FIC2_CHECK(!binding.IsLoading());

    Load(binding, FakePathA());
    FIC2_CHECK(binding.CurrentPhase() == Phase::Loading);
    FIC2_CHECK(binding.IsLoading());

    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.CurrentPhase() == Phase::Ready);
    FIC2_CHECK(binding.IsLoading());  // staged, not yet taken

    FIC2_CHECK(binding.TakePending().blocks != nullptr);
    FIC2_CHECK(binding.CurrentPhase() == Phase::Idle);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(binding.TakePending().blocks == nullptr);

    Load(binding, FakePathB());
    service.fake.Complete(1, E_FAIL);
    FIC2_CHECK(binding.CurrentPhase() == Phase::Failed);
    FIC2_CHECK(binding.IsFailedFor(FakePathB()));
    FIC2_CHECK(!binding.IsLoading());

    // Cancel keeps a failure (and its memo); it supersedes Loading and Ready.
    binding.Cancel();
    FIC2_CHECK(binding.CurrentPhase() == Phase::Failed);
    FIC2_CHECK(binding.IsFailedFor(FakePathB()));

    Load(binding, FakePathA());
    binding.Cancel();
    FIC2_CHECK(binding.CurrentPhase() == Phase::Superseded);
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[2].handle));

    Load(binding, FakePathA());
    service.fake.Complete(3, S_OK, MakeBgraImage(4, 2));
    binding.Cancel();
    FIC2_CHECK(binding.CurrentPhase() == Phase::Superseded);
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
}

FIC2_TEST(ImageAsyncBinding_RejectedRequestFailsInsteadOfLoadingForever)
{
    // ImageCore returns handle 0 (for example after shutdown) and never calls
    // back; the binding must not stay Loading.
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    service.fake.rejectNext = true;
    Load(binding, FakePathA());
    FIC2_CHECK(binding.CurrentPhase() == ImageAsyncBinding::Phase::Failed);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(binding.IsFailedFor(FakePathA()));
    FIC2_CHECK(binding.ClearFailureIfMatches(FakePathA(), true /*requireFailedHr*/));

    // A later accepted request proceeds normally.
    Load(binding, FakePathA());
    FIC2_REQUIRE(service.fake.RequestCount() == 1);
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathA()));
}

FIC2_TEST(ImageAsyncBinding_CompletionBeforeHandleStoredIsKept)
{
    // The worker finishes before RequestDecoded returns. The result is staged,
    // and the already-finished handle is neither stored nor canceled later.
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    service.fake.completeBeforeReturn = true;
    Load(binding, FakePathA());
    FIC2_REQUIRE(service.fake.RequestCount() == 1);
    FIC2_CHECK(binding.CurrentPhase() == ImageAsyncBinding::Phase::Ready);
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathA()));

    binding.Cancel();
    Load(binding, FakePathB());
    FIC2_CHECK(!service.fake.WasCanceled(service.fake.requests[0].handle));
}

FIC2_TEST(ImageAsyncBinding_LateCallbacksAfterCancelChangeNothing)
{
    // ImageCore may deliver a callback after Cancel returned. Neither a late
    // success nor a late failure may touch the superseded or a newer request.
    using Phase = ImageAsyncBinding::Phase;
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    binding.Cancel();
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    service.fake.Complete(0, E_FAIL);
    FIC2_CHECK(binding.CurrentPhase() == Phase::Superseded);
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);

    Load(binding, FakePathB());
    service.fake.Complete(0, E_FAIL);
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.CurrentPhase() == Phase::Loading);
    FIC2_CHECK(!binding.IsFailedFor(FakePathA()));

    service.fake.Complete(1, S_OK, MakeBgraImage(8, 6));
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathB()));
    // A duplicate delivery of the current request after it settled is ignored.
    service.fake.Complete(1, E_FAIL);
    FIC2_CHECK(binding.CurrentPhase() == Phase::Ready);
    FIC2_CHECK(!binding.IsFailedFor(FakePathB()));
}

FIC2_TEST(ImageAsyncBinding_MarkSettledSupersedesInFlightRequest)
{
    // The control showed cached content instead; the in-flight decode is
    // canceled and its result must not be staged later.
    using Phase = ImageAsyncBinding::Phase;
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    Load(binding, FakePathA());
    binding.MarkSettled();
    FIC2_CHECK(binding.CurrentPhase() == Phase::Superseded);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(service.fake.WasCanceled(service.fake.requests[0].handle));
    service.fake.Complete(0, S_OK, MakeBgraImage(4, 2));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);

    // Settling a failure or an idle binding changes nothing.
    Load(binding, FakePathB());
    service.fake.Complete(1, E_FAIL);
    binding.MarkSettled();
    FIC2_CHECK(binding.CurrentPhase() == Phase::Failed);
    FIC2_CHECK(binding.IsFailedFor(FakePathB()));
}

FIC2_TEST(ImageAsyncBinding_StaleFailureRacingNewRequestKeepsNewer)
{
    // Regression: a failed completion used to clear `loading` and the in-flight
    // generation after releasing the mutex, so a request installed in between
    // was left with no loading state and its own completion dropped. Here the
    // old request fails on another thread exactly while the newer one starts.
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    constexpr int kRounds = 5000;
    int lost = 0;
    for (int round = 0; round < kRounds; ++round)
    {
        const std::wstring& older = (round % 2 == 0) ? FakePathA() : FakePathB();
        const std::wstring& newer = (round % 2 == 0) ? FakePathB() : FakePathA();
        const size_t oldIndex = service.fake.RequestCount();
        Load(binding, older);

        // Both sides spin until released together, so the old failure and the
        // new request overlap instead of running one after the other.
        std::atomic<bool> ready { false };
        std::atomic<bool> go { false };
        std::thread worker([&]()
        {
            ready.store(true);
            while (!go.load())
            {
            }
            service.fake.Complete(oldIndex, E_FAIL);
        });
        while (!ready.load())
        {
        }
        go.store(true);
        Load(binding, newer);
        worker.join();

        FIC2_REQUIRE(service.fake.RequestCount() == oldIndex + 2);
        if (!binding.IsLoading())
        {
            ++lost;
        }
        service.fake.Complete(oldIndex + 1, S_OK, MakeBgraImage(8, 6));
        if (!binding.HasPendingCpuBgra8For(newer))
        {
            ++lost;
        }
        FIC2_CHECK(!binding.IsFailedFor(newer));
        (void)binding.TakePending();
        binding.ClearFailure();
    }
    FIC2_CHECK(lost == 0);
}

FIC2_TEST(ImageAsyncBinding_SelectCancelReselectStressKeepsLatest)
{
    // Rapid select / cancel / reselect with completions arriving on another
    // thread in arbitrary order; the latest request's state and result survive.
    ScopedFakeDecodeService service;
    ImageAsyncBinding binding;

    std::atomic<bool> stop { false };
    std::atomic<size_t> delivered { 0 };
    std::thread worker([&]()
    {
        size_t next = 0;
        while (!stop.load())
        {
            const size_t count = service.fake.RequestCount();
            if (next < count)
            {
                // Mix successes and failures for every superseded request.
                service.fake.Complete(next, (next % 3 == 0) ? E_FAIL : S_OK, MakeBgraImage(4, 2));
                ++next;
                delivered.store(next);
            }
        }
    });

    constexpr int kRounds = 3000;
    for (int round = 0; round < kRounds; ++round)
    {
        Load(binding, (round % 2 == 0) ? FakePathA() : FakePathB());
        if (round % 5 == 0)
        {
            binding.Cancel();
        }
        if (round % 7 == 0)
        {
            (void)binding.TakePending();
        }
    }
    stop.store(true);
    worker.join();

    // Final request, completed only after every older completion was delivered.
    binding.ClearFailure();
    Load(binding, FakePathB());
    const size_t last = service.fake.RequestCount() - 1;
    for (size_t i = delivered.load(); i < last; ++i)
    {
        service.fake.Complete(i, E_FAIL);
    }
    FIC2_CHECK(binding.CurrentPhase() == ImageAsyncBinding::Phase::Loading);
    FIC2_CHECK(!binding.IsFailedFor(FakePathB()));
    service.fake.Complete(last, S_OK, MakeBgraImage(8, 6));
    FIC2_CHECK(binding.HasPendingCpuBgra8For(FakePathB()));

    // Every superseded handle was canceled at most once.
    std::vector<ImageCore::ImageHandle> cancels = service.fake.cancels;
    std::sort(cancels.begin(), cancels.end());
    FIC2_CHECK(std::adjacent_find(cancels.begin(), cancels.end()) == cancels.end());
}

// -------------------------------------------------------------------------
// End-to-end with the real ImageCore loader
// -------------------------------------------------------------------------

FIC2_TEST(ImageAsyncBinding_RealLoaderLoadsPayloadForRequestedPath)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.smallBmp);
    FIC2_CHECK(binding.IsLoading());
    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.smallBmp); }));

    bool applied = false;
    const auto payload = Apply(binding, applied);
    FIC2_CHECK(applied);
    FIC2_CHECK(payload.sourcePath == files.smallBmp);
    FIC2_CHECK(payload.width == 4);
    FIC2_CHECK(payload.height == 2);
    FIC2_CHECK(ImageAsyncBinding::IsCpuBgra8Format(payload.format));
    FIC2_CHECK(payload.blocks && payload.blocks->size() >= payload.rowPitch * payload.height);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}

FIC2_TEST(ImageAsyncBinding_RealLoaderRecordsFailureForMissingFile)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.missingBmp);
    // The worker records the failure under the mutex and clears `loading`
    // afterwards, so wait for the complete transition before asserting.
    FIC2_REQUIRE(WaitUntil([&]()
    {
        return binding.IsFailedFor(files.missingBmp) && !binding.IsLoading();
    }));
    FIC2_CHECK(!binding.HasPendingCpuBgra8For(files.missingBmp));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
}

FIC2_TEST(ImageAsyncBinding_RealLoaderSuccessClearsPreviousFailure)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    binding.RecordFailure(files.smallBmp, E_FAIL);
    Load(binding, files.smallBmp);
    // The failure is cleared under the same lock that stages the payload.
    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.smallBmp); }));
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}

FIC2_TEST(ImageAsyncBinding_RealLoaderRapidReselectKeepsLatestAndDrains)
{
    // Rapid navigation against the real loader: many superseded requests and
    // cancels, then the latest result must arrive and ImageCore must hold no
    // leftover cancellation state.
    const auto& files = Files();
    ImageAsyncBinding binding;

    for (int round = 0; round < 200; ++round)
    {
        Load(binding, (round % 2 == 0) ? files.otherBmp : files.smallBmp);
        if (round % 4 == 0)
        {
            binding.Cancel();
        }
        if (round % 9 == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Load(binding, files.smallBmp);

    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.smallBmp); }));
    bool applied = false;
    const auto payload = Apply(binding, applied);
    FIC2_CHECK(applied);
    FIC2_CHECK(payload.sourcePath == files.smallBmp);
    FIC2_CHECK(payload.width == 4);
    FIC2_CHECK(payload.height == 2);
    FIC2_CHECK(binding.CurrentPhase() == ImageAsyncBinding::Phase::Idle);
    FIC2_CHECK(WaitUntil([&]() { return ImageCore::ImageLoader::Instance().LiveRequestCount() == 0; }));
}
