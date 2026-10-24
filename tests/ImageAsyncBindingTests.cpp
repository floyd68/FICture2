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
#include <string>
#include <thread>
#include <vector>

// Exercises ImageAsyncBinding against the real ImageCore loader with tiny BMP
// files written to a temporary directory. Waits are bounded; assertions only
// rely on outcomes that cannot be reached by a late or stale completion.

namespace
{
    constexpr auto kWaitTimeout = std::chrono::milliseconds(10000);
    constexpr auto kSettle = std::chrono::milliseconds(300);

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
        std::wstring largeBmp;   // 8 x 6
        std::wstring missingBmp;

        Fixture()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"FICture2Tests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            WriteBmp(dir / L"small.bmp", 4, 2);
            WriteBmp(dir / L"large.bmp", 8, 6);

            smallBmp = CommonUtil::NormalizePath((dir / L"small.bmp").wstring());
            largeBmp = CommonUtil::NormalizePath((dir / L"large.bmp").wstring());
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
}

FIC2_TEST(ImageAsyncBinding_LoadsPayloadForRequestedPath)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.smallBmp);
    FIC2_CHECK(binding.IsLoading());
    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.smallBmp); }));

    ImageAsyncBinding::Payload applied {};
    const bool didApply = binding.PollAndApply([&](ImageAsyncBinding::Payload&& payload)
    {
        applied = std::move(payload);
    });

    FIC2_CHECK(didApply);
    FIC2_CHECK(applied.sourcePath == files.smallBmp);
    FIC2_CHECK(applied.width == 4);
    FIC2_CHECK(applied.height == 2);
    FIC2_CHECK(ImageAsyncBinding::IsCpuBgra8Format(applied.format));
    FIC2_CHECK(applied.blocks && applied.blocks->size() >= applied.rowPitch * applied.height);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}

FIC2_TEST(ImageAsyncBinding_PollWithoutPendingDoesNothing)
{
    ImageAsyncBinding binding;
    bool called = false;
    FIC2_CHECK(!binding.PollAndApply([&](ImageAsyncBinding::Payload&&) { called = true; }));
    FIC2_CHECK(!called);
    FIC2_CHECK(!binding.PollAndApply({}));
}

FIC2_TEST(ImageAsyncBinding_RecordsFailureForMissingFile)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.missingBmp);
    FIC2_REQUIRE(WaitUntil([&]() { return binding.IsFailedFor(files.missingBmp); }));
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(!binding.HasPendingCpuBgra8For(files.missingBmp));
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
}

FIC2_TEST(ImageAsyncBinding_NewerRequestSupersedesOlder)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.smallBmp);
    Load(binding, files.largeBmp);

    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.largeBmp); }));
    ImageAsyncBinding::Payload applied {};
    FIC2_REQUIRE(binding.PollAndApply([&](ImageAsyncBinding::Payload&& payload)
    {
        applied = std::move(payload);
    }));
    FIC2_CHECK(applied.sourcePath == files.largeBmp);
    FIC2_CHECK(applied.width == 8);
    FIC2_CHECK(applied.height == 6);

    // A late completion of the superseded request must never surface.
    std::this_thread::sleep_for(kSettle);
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
    FIC2_CHECK(!binding.IsLoading());
}

FIC2_TEST(ImageAsyncBinding_CancelDropsInFlightResult)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    Load(binding, files.smallBmp);
    binding.Cancel();
    FIC2_CHECK(!binding.IsLoading());

    std::this_thread::sleep_for(kSettle);
    FIC2_CHECK(binding.TakePending().blocks == nullptr);
    FIC2_CHECK(!binding.IsLoading());
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}

FIC2_TEST(ImageAsyncBinding_RegistryInvalidateAllCancelsLiveBindings)
{
    const auto& files = Files();
    ImageAsyncBinding first;
    ImageAsyncBinding second;

    Load(first, files.smallBmp);
    Load(second, files.largeBmp);
    ImageAsyncBindingRegistry::Instance().InvalidateAll();
    FIC2_CHECK(!first.IsLoading());
    FIC2_CHECK(!second.IsLoading());

    std::this_thread::sleep_for(kSettle);
    FIC2_CHECK(first.TakePending().blocks == nullptr);
    FIC2_CHECK(second.TakePending().blocks == nullptr);
}

FIC2_TEST(ImageAsyncBinding_DestroyWhileInFlightIsSafe)
{
    const auto& files = Files();
    {
        ImageAsyncBinding binding;
        Load(binding, files.largeBmp);
    }

    // The completion only holds the shared state; it must not touch the
    // destroyed binding. A subsequent binding still loads normally.
    std::this_thread::sleep_for(kSettle);
    ImageAsyncBinding next;
    Load(next, files.smallBmp);
    FIC2_CHECK(WaitUntil([&]() { return next.HasPendingCpuBgra8For(files.smallBmp); }));
}

FIC2_TEST(ImageAsyncBinding_FailureBookkeeping)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    binding.RecordFailure(files.smallBmp, E_FAIL);
    FIC2_CHECK(binding.IsFailedFor(files.smallBmp));
    FIC2_CHECK(!binding.IsFailedFor(files.largeBmp));
    FIC2_CHECK(!binding.ClearFailureIfMatches(files.largeBmp));
    FIC2_CHECK(binding.ClearFailureIfMatches(files.smallBmp, true /*requireFailedHr*/));
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));

    // A recorded success HRESULT does not satisfy requireFailedHr.
    binding.RecordFailure(files.smallBmp, S_OK);
    FIC2_CHECK(!binding.ClearFailureIfMatches(files.smallBmp, true));
    FIC2_CHECK(binding.ClearFailureIfMatches(files.smallBmp, false));

    binding.RecordFailure(files.smallBmp, E_FAIL);
    binding.ClearFailure();
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}

FIC2_TEST(ImageAsyncBinding_SuccessClearsPreviousFailure)
{
    const auto& files = Files();
    ImageAsyncBinding binding;

    binding.RecordFailure(files.smallBmp, E_FAIL);
    Load(binding, files.smallBmp);
    FIC2_REQUIRE(WaitUntil([&]() { return binding.HasPendingCpuBgra8For(files.smallBmp); }));
    FIC2_CHECK(!binding.IsFailedFor(files.smallBmp));
}
