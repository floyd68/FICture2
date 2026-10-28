#include "TestHarness.h"

#include "ArchiveCancelTestSupport.h"
#include "CommonUtil.h"
#include "ImageAsyncBinding.h"

#include "ImageCore/IPathByteSource.h"
#include "ImageCore/ImageLoader.h"

#include <thread>

// Request-owned cancellation of archive reads, end to end: FloarPathByteSource
// forwards the token to Floar (pre-cancellation, waiting for a cold or shared
// reader open, extraction), and through ImageCore a canceled thumbnail or
// full-resolution read of folder A returns through its token only, publishes no
// bytes, starts no decode or retry, and lets folder B's first read run next.
// Archives are faked through the ArchiveReaderCache opener seam, so nothing
// depends on the machine's files; every wait is bounded.

namespace
{
    using namespace ArchiveCancelTest;
    using Kind = RecordingSource::Event::Kind;

    // One injected archive cache (opener -> GateArchive) and a recording
    // FloarPathByteSource on it, optionally installed as ImageCore's source.
    // Teardown releases every barrier, waits for reads in flight, restores the
    // previous source and shuts the cache down (leaking it if that fails, so a
    // stuck worker never touches freed state).
    struct Env
    {
        struct Parts
        {
            GateArchive archive {};
            GateOpener opener { archive };
            Floar::ArchiveReaderCache cache;
            RecordingSource source { cache };

            Parts()
                : cache(Floar::ArchiveReaderCache::kDefaultCapacity, opener.Make())
            {
            }
        };

        std::filesystem::path dir;
        Parts* parts { new Parts() };
        GateArchive& archive { parts->archive };
        GateOpener& opener { parts->opener };
        Floar::ArchiveReaderCache& cache { parts->cache };
        RecordingSource& source { parts->source };
        bool installed { false };
        ImageCore::IPathByteSource* previous { nullptr };

        explicit Env(const std::wstring& tag, bool installInImageCore)
            : dir(MakeTempDir(tag))
        {
            (void)CountingDecoderFactory::Instance();
            if (installInImageCore)
            {
                previous = ImageCore::GetPathByteSource();
                ImageCore::SetPathByteSource(&source);
                installed = true;
            }
        }

        ~Env()
        {
            archive.Release();
            opener.Release();
            const bool idle = WaitUntil([&] { return source.InFlight() == 0; });
            if (installed)
            {
                ImageCore::SetPathByteSource(previous);
            }
            if (idle && cache.Shutdown(5s))
            {
                delete parts;
            }
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        // Display path of `entry` in archive `archiveName` (registered in the fake).
        std::wstring ArchiveEntry(const std::wstring& archiveName, const std::wstring& entry, bool blocking)
        {
            archive.AddEntry(entry, blocking);
            return CommonUtil::NormalizePath((dir / archiveName).wstring() + L"\\" + entry);
        }

        // A plain file the scheduler reads from disk. It is read back here
        // once, on the test thread, outside every bounded wait: the first open
        // of a just-written file can block in CreateFile for many seconds
        // (most likely an on-access scan of the new file) when it comes from
        // the thumbnail prefetch thread, which runs in background mode (very
        // low CPU and I/O priority), on a busy machine; that stalls the
        // reads queued behind it in its lane too. The tests time cancellation
        // and ordering, not that first open.
        std::wstring LocalFile(const std::wstring& name)
        {
            const std::filesystem::path path = dir / name;
            WriteFile(path, BytesFor(name));
            std::ifstream in(path, std::ios::binary);
            std::vector<char> readBack(BytesFor(name).size());
            in.read(readBack.data(), static_cast<std::streamsize>(readBack.size()));
            return CommonUtil::NormalizePath(path.wstring());
        }
    };

    ImageCore::ImageRequest Thumb(const std::wstring& path)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::Thumbnail);
        request.targetSize = ImageCore::Size { 64.0f, 64.0f };
        request.allowGpuCompressedDDS = false;
        return request;
    }

    ImageCore::ImageRequest Full(const std::wstring& path)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::FullResolution);
        request.allowGpuCompressedDDS = false;
        return request;
    }

    // Polls the binding until a payload is applied; returns its source path.
    std::wstring WaitApplied(ImageAsyncBinding& binding)
    {
        std::wstring applied;
        (void)WaitUntil([&]
        {
            return binding.PollAndApply([&](ImageAsyncBinding::Payload&& payload)
            {
                applied = payload.sourcePath;
            });
        });
        return applied;
    }

    // Runs env.source.ReadAll(path, stop) on its own thread. Its destructor
    // opens every barrier before joining, so a failed check cannot hang.
    struct AsyncRead
    {
        Env& env;
        std::stop_source stop {};
        std::atomic<bool> done { false };
        std::vector<uint8_t> bytes {};
        std::thread thread {};

        AsyncRead(Env& owner, const std::wstring& path)
            : env(owner)
        {
            thread = std::thread([this, path]
            {
                bytes = env.source.ReadAll(path, stop.get_token());
                done.store(true);
            });
        }

        ~AsyncRead()
        {
            env.archive.Release();
            env.opener.Release();
            if (thread.joinable())
            {
                thread.join();
            }
        }
    };
}

// -------------------------------------------------------------------------
// FloarPathByteSource adapter (injected cache)
// -------------------------------------------------------------------------

FIC2_TEST(ArchiveReadCancel_PreCanceledReadStartsNothing)
{
    Env env(L"precancel", false);
    const std::wstring entry = env.ArchiveEntry(L"A.zip", L"a.fic2c", false);
    const std::wstring file = env.LocalFile(L"plain.fic2c");

    std::stop_source stop;
    stop.request_stop();
    FIC2_CHECK(env.source.ReadAll(entry, stop.get_token()).empty());
    FIC2_CHECK(env.source.ReadAll(file, stop.get_token()).empty());

    // No reader open, no extraction, no worker.
    FIC2_CHECK(env.cache.OpenCount() == 0);
    FIC2_CHECK(env.cache.WorkerCount() == 0);
    FIC2_CHECK(env.archive.Extractions().empty());
}

FIC2_TEST(ArchiveReadCancel_UntokenedAndUnstoppedReadsReturnTheBytes)
{
    Env env(L"untokened", false);
    const std::wstring entry = env.ArchiveEntry(L"A.zip", L"a.fic2c", false);
    const std::wstring file = env.LocalFile(L"plain.fic2c");

    // The legacy overload and a live token both read archives and plain files.
    const FloarPathByteSource& source = env.source;
    FIC2_CHECK(source.ReadAll(entry) == BytesFor(L"a.fic2c"));
    std::stop_source stop;
    FIC2_CHECK(source.ReadAll(entry, stop.get_token()) == BytesFor(L"a.fic2c"));
    FIC2_CHECK(source.ReadAll(file, stop.get_token()) == BytesFor(L"plain.fic2c"));
    FIC2_CHECK(env.cache.OpenCount() == 1);   // the reader is cached
    FIC2_CHECK(source.ResolveHostPath(entry) == CommonUtil::NormalizePath((env.dir / L"A.zip").wstring()));
}

FIC2_TEST(ArchiveReadCancel_StopReleasesExtractionAtItsBarrier)
{
    Env env(L"extract", false);
    const std::wstring entry = env.ArchiveEntry(L"A.zip", L"a.fic2c", true);

    AsyncRead read(env, entry);
    FIC2_REQUIRE(env.archive.WaitBlocked(L"a.fic2c"));
    read.stop.request_stop();
    FIC2_REQUIRE(WaitUntil([&] { return read.done.load(); }));

    // Released through the token only, and nothing partial is returned.
    FIC2_CHECK(!env.archive.Released());
    FIC2_CHECK(read.bytes.empty());
    FIC2_CHECK(env.archive.Done(L"a.fic2c").returnedByStop);
}

FIC2_TEST(ArchiveReadCancel_StopReleasesCallerWaitingForColdOpen)
{
    Env env(L"coldopen", false);
    env.opener.blockOpen = true;
    const std::wstring entry = env.ArchiveEntry(L"A.zip", L"a.fic2c", false);

    AsyncRead read(env, entry);
    FIC2_REQUIRE(env.opener.WaitEntered(1));
    FIC2_REQUIRE(WaitUntil([&] { return env.cache.WaitingCount() == 1; }));
    read.stop.request_stop();
    FIC2_REQUIRE(WaitUntil([&] { return read.done.load(); }));
    FIC2_CHECK(read.bytes.empty());

    // The caller was the open's only interest: the open is stopped through its
    // own token (not released at the barrier), publishes nothing and is retired.
    FIC2_CHECK(WaitUntil([&] { return env.opener.InFlight() == 0; }));
    FIC2_CHECK(env.opener.StopSeen() == 1);
    FIC2_CHECK(WaitUntil([&] { return env.cache.RunningOpenCount() == 0; }));
    FIC2_CHECK(env.archive.Extractions().empty());
}

FIC2_TEST(ArchiveReadCancel_StoppedCallerLeavesSharedOpenToOthers)
{
    Env env(L"contention", false);
    env.opener.blockOpen = true;
    const std::wstring first = env.ArchiveEntry(L"A.zip", L"a.fic2c", false);
    const std::wstring second = env.ArchiveEntry(L"A.zip", L"b.fic2c", false);

    AsyncRead canceled(env, first);
    AsyncRead waiting(env, second);
    FIC2_REQUIRE(env.opener.WaitEntered(1));
    FIC2_REQUIRE(WaitUntil([&] { return env.cache.WaitingCount() == 2; }));

    // The stopped caller leaves at once; the open keeps running for the other.
    canceled.stop.request_stop();
    FIC2_REQUIRE(WaitUntil([&] { return canceled.done.load(); }));
    FIC2_CHECK(canceled.bytes.empty());
    FIC2_CHECK(env.cache.RunningOpenCount() == 1);
    FIC2_CHECK(env.cache.WaitingCount() == 1);
    FIC2_CHECK(env.opener.StopSeen() == 0);
    FIC2_CHECK(!waiting.done.load());

    env.opener.Release();
    FIC2_REQUIRE(WaitUntil([&] { return waiting.done.load(); }));
    FIC2_CHECK(waiting.bytes == BytesFor(L"b.fic2c"));
    FIC2_CHECK(env.cache.OpenCount() == 1);   // one shared open, no rescan
    FIC2_CHECK(env.archive.Done(L"a.fic2c").entry.empty());   // never extracted
}

// -------------------------------------------------------------------------
// Through ImageCore (real ImageLoader, bindings as the tiles use them)
// -------------------------------------------------------------------------

// Folder A's thumbnail read blocks in extraction; the tile is recycled for
// folder B. A's read returns through its token, and B's read runs next.
FIC2_TEST(ArchiveReadCancel_ThumbnailNavigationDuringExtraction)
{
    Env env(L"thumbextract", true);
    const std::wstring a = env.ArchiveEntry(L"A.zip", L"a.fic2c", true);
    const std::wstring b = env.LocalFile(L"b.fic2c");

    ImageAsyncBinding binding;
    binding.RequestLoad(Thumb(a), a);
    FIC2_REQUIRE(env.archive.WaitBlocked(L"a.fic2c"));

    binding.RequestLoad(Thumb(b), b);
    FIC2_CHECK(WaitApplied(binding) == b);
    // B (a local file) is read in ImageCore's Local lane, so it does not wait
    // for A's archive read to return (floyd68/ImageCore#15); wait for A here.
    FIC2_REQUIRE(WaitUntil([&] { return env.source.Returned(a).path == a; }));

    FIC2_CHECK(!env.archive.Released());
    FIC2_CHECK(env.archive.Done(L"a.fic2c").returnedByStop);
    const auto aRead = env.source.Returned(a);
    FIC2_CHECK(aRead.path == a);
    FIC2_CHECK(aRead.bytes == 0);
    FIC2_CHECK(aRead.stopRequested);
    // No decode, no path fallback and no retry for A.
    FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(a) == 0);
    FIC2_CHECK(env.source.Count(Kind::Start, a) == 1);
    FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(b) == 1);
}

// The same while A's archive is still being opened (cold reader open).
FIC2_TEST(ArchiveReadCancel_ThumbnailNavigationDuringColdOpen)
{
    Env env(L"thumbopen", true);
    env.opener.blockOpen = true;
    const std::wstring a = env.ArchiveEntry(L"A.zip", L"a.fic2c", false);
    const std::wstring b = env.LocalFile(L"b.fic2c");

    ImageAsyncBinding binding;
    binding.RequestLoad(Thumb(a), a);
    FIC2_REQUIRE(env.opener.WaitEntered(1));
    FIC2_REQUIRE(WaitUntil([&] { return env.cache.WaitingCount() == 1; }));

    binding.RequestLoad(Thumb(b), b);
    FIC2_CHECK(WaitApplied(binding) == b);
    // B does not wait for A's read (separate ImageCore lanes); wait for A here.
    FIC2_REQUIRE(WaitUntil([&] { return env.source.Returned(a).path == a; }));

    const auto aRead = env.source.Returned(a);
    FIC2_CHECK(aRead.path == a);
    FIC2_CHECK(aRead.bytes == 0);
    FIC2_CHECK(WaitUntil([&] { return env.opener.InFlight() == 0; }));
    FIC2_CHECK(env.opener.StopSeen() == 1);
    FIC2_CHECK(env.archive.Extractions().empty());
    FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(a) == 0);
    FIC2_CHECK(env.source.Count(Kind::Start, a) == 1);
}

// A full-resolution archive read (main-image reader) is canceled the same way.
FIC2_TEST(ArchiveReadCancel_FullResolutionCancelDuringExtraction)
{
    Env env(L"fullres", true);
    const std::wstring a = env.ArchiveEntry(L"A.zip", L"a.fic2c", true);
    const std::wstring b = env.ArchiveEntry(L"A.zip", L"b.fic2c", false);

    ImageAsyncBinding binding;
    binding.RequestLoad(Full(a), a);
    FIC2_REQUIRE(env.archive.WaitBlocked(L"a.fic2c"));
    binding.Cancel();
    FIC2_REQUIRE(env.archive.WaitDone(L"a.fic2c"));
    FIC2_CHECK(env.archive.Done(L"a.fic2c").returnedByStop);
    FIC2_CHECK(!env.archive.Released());

    binding.RequestLoad(Full(b), b);
    FIC2_CHECK(WaitApplied(binding) == b);

    const auto aRead = env.source.Returned(a);
    FIC2_CHECK(aRead.path == a);
    FIC2_CHECK(aRead.bytes == 0);
    FIC2_CHECK(aRead.stopRequested);
    FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(a) == 0);
    FIC2_CHECK(env.source.Count(Kind::Start, a) == 1);
    FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(b) == 1);
}

// Cancellation racing natural completion: A's barrier opens while the tile
// moves to B. Either outcome is allowed for A, but a canceled read never
// decodes, and only B ever reaches the tile.
FIC2_TEST(ArchiveReadCancel_CancelNearNaturalCompletion)
{
    Env env(L"race", true);
    ImageAsyncBinding binding;
    for (int i = 0; i < 20; ++i)
    {
        const std::wstring entry = L"a" + std::to_wstring(i) + L".fic2c";
        const std::wstring a = env.ArchiveEntry(L"A.zip", entry, true);
        const std::wstring b = env.LocalFile(L"b" + std::to_wstring(i) + L".fic2c");

        binding.RequestLoad(Thumb(a), a);
        FIC2_REQUIRE(env.archive.WaitBlocked(entry));
        std::thread release([&] { env.archive.ReleaseEntry(entry); });
        binding.RequestLoad(Thumb(b), b);
        release.join();

        FIC2_CHECK(WaitApplied(binding) == b);
        FIC2_REQUIRE(WaitUntil([&] { return env.source.Returned(a).path == a; }));
        const auto aRead = env.source.Returned(a);
        if (aRead.bytes == 0)
        {
            FIC2_CHECK(aRead.stopRequested);
            FIC2_CHECK(CountingDecoderFactory::Instance().Decodes(a) == 0);
        }
        FIC2_CHECK(env.source.Count(Kind::Start, a) == 1);
    }
    // Let a late decode of an uncanceled A finish before the next test.
    FIC2_CHECK(WaitUntil([] { return ImageCore::ImageLoader::Instance().LiveRequestCount() == 0; }));
}
