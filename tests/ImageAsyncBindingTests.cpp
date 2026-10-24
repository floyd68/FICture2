#include "TestHarness.h"

#include "CommonUtil.h"
#include "ImageAsyncBinding.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageLoader.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
        };

        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback) override
        {
            const ImageCore::ImageHandle handle = ++m_nextHandle;
            requests.push_back({ handle, request, std::move(callback) });
            return handle;
        }

        void Cancel(ImageCore::ImageHandle handle) override
        {
            cancels.push_back(handle);
        }

        // Delivers the completion of request `index` (as a worker thread would).
        void Complete(size_t index, HRESULT hr, ImageCore::DecodedImage image = {})
        {
            requests.at(index).callback(hr, std::move(image));
        }

        bool WasCanceled(ImageCore::ImageHandle handle) const
        {
            for (const auto canceled : cancels)
            {
                if (canceled == handle)
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<Request> requests;
        std::vector<ImageCore::ImageHandle> cancels;

    private:
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
        std::wstring missingBmp;

        Fixture()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2Tests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            WriteBmp(dir / L"small.bmp", 4, 2);

            smallBmp = CommonUtil::NormalizePath((dir / L"small.bmp").wstring());
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
