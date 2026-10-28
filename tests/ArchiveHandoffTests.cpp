#include "TestHarness.h"

#include "ArchiveEntryHandoff.h"
#include "CommonUtil.h"
#include "FloarPathByteSource.h"
#include "ImageAsyncBinding.h"
#include "TestArchives.h"   // Floar's solid 7z / ZIP fixture writers

#include "ArchiveCursorPool.h"
#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "VirtualPath.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/IImageDecoderFactory.h"
#include "ImageCore/IPathByteSource.h"
#include "ImageCore/ImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Batched solid-archive thumbnail reads (#43): ArchiveEntryHandoff behind
// FloarPathByteSource. A demanded thumbnail read runs one forward pass that
// parks the other demanded entries before it, later reads take parked bytes
// (no archive access) or continue the retained cursor, backward demand
// restarts once, the parked bytes stay within their budget (oversize entries
// are never parked), withdrawn demands and replaced archives never receive
// stale bytes, a stopped read leaves everything serviceable, main images read
// alone at High priority, and plain files, ZIP and BA2-like readers read as
// before. Generated solid 7z archives (libarchive's writer) or a scripted
// reader; work is counted with Floar's cursor-pool statistics and the
// handoff's counters, never with timing.

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    using namespace std::chrono_literals;
    using Kind = ArchiveEntryHandoff::DemandKind;
    using Demand = ArchiveEntryHandoff::Demand;

    template <typename Predicate>
    bool WaitUntil(Predicate predicate, std::chrono::milliseconds bound = 20s)
    {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    std::filesystem::path MakeTempDir(const std::wstring& tag)
    {
        static std::atomic<unsigned> counter { 0 };
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FICture2Handoff_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" +
                std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter.fetch_add(1)));
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A solid 7z: kOther entries in another folder, then kDeep in a deep
    // folder, in that archive order. Every entry differs and compresses, so
    // reading one decompresses every entry before it.
    constexpr int kOther = 8;
    constexpr int kDeep = 32;
    constexpr int kEntries = kOther + kDeep;
    constexpr size_t kEntrySize = 16 * 1024;

    // Entry `i` of the deep folder (archive index kOther + i).
    std::wstring DeepName(int i, const wchar_t* extension = L".dds")
    {
        wchar_t buffer[96] {};
        swprintf_s(buffer, L"Data/Textures/Deep/Folder/t%02d%s", i, extension);
        return buffer;
    }

    std::wstring OtherName(int i)
    {
        wchar_t buffer[64] {};
        swprintf_s(buffer, L"Data/Other/o%02d.bin", i);
        return buffer;
    }

    // Its first four bytes are the archive index and the tag, so a reader
    // can tell whose (and which version's) bytes it got.
    std::string EntryData(int index, char tag, size_t size)
    {
        std::mt19937 random(static_cast<unsigned>(index) * 13 + static_cast<unsigned>(tag));
        std::string data(size, '\0');
        for (char& c : data)
        {
            c = static_cast<char>(tag + (random() % 16));
        }
        data[0] = static_cast<char>(index);
        data[1] = tag;
        return data;
    }

    std::vector<uint8_t> Bytes(const std::string& text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    using SizeOf = std::function<size_t(int deepIndex)>;

    bool WriteFixture(const std::filesystem::path& path, char tag, const SizeOf& sizeOf, const wchar_t* extension)
    {
        std::vector<std::pair<std::wstring, std::string>> files;
        for (int i = 0; i < kOther; ++i)
        {
            files.emplace_back(OtherName(i), EntryData(i, tag, kEntrySize));
        }
        for (int i = 0; i < kDeep; ++i)
        {
            files.emplace_back(DeepName(i, extension), EntryData(kOther + i, tag, sizeOf ? sizeOf(i) : kEntrySize));
        }
        return FloarTest::WriteSolid7z(path, files, 1);
    }

    // A generated solid 7z read through FloarPathByteSource with a private
    // cursor pool, archive cache and handoff. Teardown shuts the cache down
    // (leaking it when that fails, so a stuck worker touches no freed state).
    struct Env
    {
        std::filesystem::path dir;
        std::filesystem::path archive;
        SizeOf sizeOf;
        const wchar_t* extension;
        bool written { false };
        std::shared_ptr<Floar::ArchiveCursorPool> pool;
        Floar::ArchiveReaderCache* cache { nullptr };
        ArchiveEntryHandoff handoff;
        FloarPathByteSource source;

        explicit Env(const std::wstring& tag, ArchiveEntryHandoff::Limits limits = {}, Floar::ArchiveCursorLimits cursorLimits = {},
            SizeOf sizes = {}, const wchar_t* ext = L".dds")
            : dir(MakeTempDir(tag))
            , archive(dir / L"solid.7z")
            , sizeOf(std::move(sizes))
            , extension(ext)
            , written(WriteFixture(archive, 'a', sizeOf, extension))
            , pool(std::make_shared<Floar::ArchiveCursorPool>(cursorLimits))
            , cache(MakeCache(pool))
            , handoff(limits)
            , source(*cache, handoff)
        {
        }

        ~Env()
        {
            if (cache->Shutdown(5s))
            {
                delete cache;
            }
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        static Floar::ArchiveReaderCache* MakeCache(const std::shared_ptr<Floar::ArchiveCursorPool>& cursorPool)
        {
            return new Floar::ArchiveReaderCache(4, [cursorPool](const std::filesystem::path& path, std::stop_token stop)
            {
                return std::shared_ptr<Floar::IArchiveReader>(Floar::ArchiveReaderFactory::Open(path, stop, cursorPool));
            });
        }

        std::wstring Path(int deep) const
        {
            return archive.wstring() + L"\\" + DeepName(deep, extension);
        }

        std::vector<uint8_t> Expected(int deep, char tag = 'a') const
        {
            return Bytes(EntryData(kOther + deep, tag, sizeOf ? sizeOf(deep) : kEntrySize));
        }

        std::vector<uint8_t> Read(int deep, std::stop_token stop = {}) const
        {
            return source.ReadAll(Path(deep), stop);
        }

        Demand Want(int deep, Kind kind = Kind::Thumbnail)
        {
            return handoff.Register(Path(deep), kind);
        }

        std::vector<Demand> Want(int first, int last)
        {
            std::vector<Demand> demands;
            for (int i = first; i <= last; ++i)
            {
                demands.push_back(Want(i));
            }
            return demands;
        }
    };

    // An archive reader whose passes are scripted: entries in a fixed archive
    // order, a hook before each delivery, and a record of every pass.
    class ScriptedReader final : public Floar::IArchiveReader
    {
    public:
        struct Pass
        {
            std::vector<std::wstring> paths {};
            Floar::ExtractPriority priority { Floar::ExtractPriority::Normal };
        };

        bool expensive { true };
        std::wstring format { L"scripted" };
        // Before delivering the entry at this archive order.
        std::function<void(size_t order)> beforeEntry {};

        explicit ScriptedReader(std::vector<std::wstring> names)
        {
            for (const auto& name : names)
            {
                m_index.emplace(Floar::ArchiveInnerPathKey(name), m_entries.size());
                m_entries.push_back({ name, 64, 64, false, 0 });
            }
        }

        static std::vector<uint8_t> BytesOf(const std::wstring& entry)
        {
            std::vector<uint8_t> bytes(64, 0);
            for (size_t i = 0; i < entry.size() && i < bytes.size(); ++i)
            {
                bytes[i] = static_cast<uint8_t>(entry[i] & 0xFF);
            }
            return bytes;
        }

        const std::vector<Floar::ArchiveEntry>& Entries() const override { return m_entries; }
        std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override { return BytesOf(entryPath); }

        ExtractResult ExtractForward(const std::vector<std::wstring>& entryPaths, std::stop_token stop,
            const ForwardSink& onEntry, const ExtractOptions& options) override
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_passes.push_back({ entryPaths, options.priority });
            }
            std::vector<std::pair<size_t, size_t>> order;   // (archive order, request index)
            for (size_t i = 0; i < entryPaths.size(); ++i)
            {
                order.emplace_back(m_index.at(Floar::ArchiveInnerPathKey(entryPaths[i])), i);
            }
            std::sort(order.begin(), order.end());
            for (const auto& [position, index] : order)
            {
                if (stop.stop_requested())
                {
                    return ExtractResult::Stopped;
                }
                if (beforeEntry)
                {
                    beforeEntry(position);
                }
                if (stop.stop_requested())
                {
                    return ExtractResult::Stopped;
                }
                if (!onEntry(entryPaths[index], BytesOf(entryPaths[index])))
                {
                    return ExtractResult::EndedBySink;
                }
            }
            return ExtractResult::Completed;
        }

        bool HasExpensiveRandomAccess() const override { return expensive; }
        size_t ArchiveOrder(const std::wstring& entryPath) const override
        {
            if (!expensive)
            {
                return kNoArchiveOrder;   // BA2/BSA: no meaningful order
            }
            auto it = m_index.find(Floar::ArchiveInnerPathKey(entryPath));
            return it == m_index.end() ? kNoArchiveOrder : it->second;
        }
        bool HasEntry(const std::wstring& entryPath) override { return m_index.count(Floar::ArchiveInnerPathKey(entryPath)) != 0; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return format; }

        std::vector<Pass> Passes() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_passes;
        }

    private:
        std::vector<Floar::ArchiveEntry> m_entries {};
        std::map<std::wstring, size_t> m_index {};
        mutable std::mutex m_mutex {};
        std::vector<Pass> m_passes {};
    };

    // A scripted archive behind a cache, with its own handoff.
    struct ScriptedEnv
    {
        std::filesystem::path dir;
        std::filesystem::path archive;
        std::shared_ptr<ScriptedReader> reader;
        Floar::ArchiveReaderCache* cache { nullptr };
        ArchiveEntryHandoff handoff;
        FloarPathByteSource source;

        explicit ScriptedEnv(const std::wstring& tag, int count, const wchar_t* archiveName = L"scripted.7z", ArchiveEntryHandoff::Limits limits = {})
            : dir(MakeTempDir(tag))
            , archive(dir / archiveName)
            , reader(std::make_shared<ScriptedReader>(Names(count)))
            , cache(new Floar::ArchiveReaderCache(4, [r = reader](const std::filesystem::path&, std::stop_token) { return std::shared_ptr<Floar::IArchiveReader>(r); }))
            , handoff(limits)
            , source(*cache, handoff)
        {
            std::ofstream(archive, std::ios::binary) << "x";   // a file for the cache's change check
        }

        ~ScriptedEnv()
        {
            if (cache->Shutdown(5s))
            {
                delete cache;
            }
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        static std::wstring Name(int i)
        {
            return L"f/e" + std::to_wstring(100 + i) + L".dds";
        }

        static std::vector<std::wstring> Names(int count)
        {
            std::vector<std::wstring> names;
            for (int i = 0; i < count; ++i)
            {
                names.push_back(Name(i));
            }
            return names;
        }

        std::wstring Path(int i) const { return archive.wstring() + L"\\" + Name(i); }
        std::vector<uint8_t> Read(int i, std::stop_token stop = {}) const { return source.ReadAll(Path(i), stop); }
        Demand Want(int i, Kind kind = Kind::Thumbnail) { return handoff.Register(Path(i), kind); }
    };
}

// -------------------------------------------------------------------------
// Shared forward pass, continuation and restart (generated solid 7z)
// -------------------------------------------------------------------------

// A demanded window read in reverse archive order (as visible-first request
// order may be): the first read's pass extracts the window once and parks the
// entries before the requested one; every other read is served from parked
// bytes. One archive open and one scan up to the last entry in all (reading
// them one by one through the VFS reopens the archive for every read that is
// behind the cursor: 16 opens).
FIC2_TEST(ArchiveHandoff_SharedPassServesTheDemandedWindow)
{
    Env env(L"shared");
    FIC2_REQUIRE(env.written);
    auto window = env.Want(0, 15);

    FIC2_CHECK(env.Read(15) == env.Expected(15));
    auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.passes == 1);
    FIC2_CHECK(stats.parked == 15);
    FIC2_CHECK(stats.parkedEntries == 15);
    for (int i = 14; i >= 0; --i)
    {
        FIC2_CHECK(env.Read(i) == env.Expected(i));
    }
    stats = env.handoff.CurrentStats();
    const auto cursors = env.pool->Stats();
    std::printf("  16 reads in reverse order: %llu pass(es), %llu hits, %llu archive open(s), %llu headers read\n",
        static_cast<unsigned long long>(stats.passes), static_cast<unsigned long long>(stats.hits),
        static_cast<unsigned long long>(cursors.opens), static_cast<unsigned long long>(cursors.headersRead));
    FIC2_CHECK(stats.passes == 1);
    FIC2_CHECK(stats.hits == 15);
    FIC2_CHECK(stats.parkedEntries == 0);
    FIC2_CHECK(stats.parkedBytes == 0);
    FIC2_CHECK(cursors.opens == 1);
    FIC2_CHECK(cursors.headersRead == static_cast<std::uint64_t>(kOther + 16));
}

// Forward demand continues one retained cursor (no reopen, no replay), also
// across a viewport change; demand behind the cursor restarts once and that
// pass collects every demanded entry before the requested one; an evicted
// cursor restarts too.
FIC2_TEST(ArchiveHandoff_ForwardDemandContinuesAndBackwardRestartsOnce)
{
    Env env(L"continue");
    FIC2_REQUIRE(env.written);

    // Window A, read in archive order: each read continues the cursor.
    {
        auto window = env.Want(0, 7);
        for (int i = 0; i <= 7; ++i)
        {
            FIC2_CHECK(env.Read(i) == env.Expected(i));
        }
    }
    auto cursors = env.pool->Stats();
    FIC2_CHECK(cursors.opens == 1);
    FIC2_CHECK(cursors.resumes == 7);
    FIC2_CHECK(env.handoff.CurrentStats().demands == 0);   // window A withdrawn

    // Scrolled forward: window B, the visible middle first, then the rest.
    {
        auto window = env.Want(8, 15);
        FIC2_CHECK(env.Read(12) == env.Expected(12));   // continues, parks 8..11
        for (int i : { 8, 9, 10, 11, 13, 14, 15 })
        {
            FIC2_CHECK(env.Read(i) == env.Expected(i));
        }
    }
    cursors = env.pool->Stats();
    auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(cursors.opens == 1);
    FIC2_CHECK(cursors.restarts == 0);
    FIC2_CHECK(cursors.headersRead == static_cast<std::uint64_t>(kOther + 16));   // nothing replayed
    FIC2_CHECK(stats.hits == 4);

    // Scrolled back: window C is behind the cursor. Its first read restarts
    // once and parks the rest of the window before it.
    {
        auto window = env.Want(2, 5);
        FIC2_CHECK(env.Read(5) == env.Expected(5));
        for (int i : { 2, 3, 4 })
        {
            FIC2_CHECK(env.Read(i) == env.Expected(i));
        }
    }
    cursors = env.pool->Stats();
    FIC2_CHECK(cursors.opens == 2);
    FIC2_CHECK(cursors.restarts == 1);
    FIC2_CHECK(env.handoff.CurrentStats().hits == 4 + 3);

    // An evicted cursor (TrimIdle) restarts the next read; a retained one continues.
    {
        auto window = env.Want(20, 21);
        FIC2_CHECK(env.pool->TrimIdle() >= 1);
        FIC2_CHECK(env.Read(20) == env.Expected(20));
        FIC2_CHECK(env.pool->Stats().opens == 3);
        FIC2_CHECK(env.Read(21) == env.Expected(21));
        FIC2_CHECK(env.pool->Stats().opens == 3);
    }
    std::printf("  opens %llu, resumes %llu, restarts %llu, headers %llu\n",
        static_cast<unsigned long long>(env.pool->Stats().opens), static_cast<unsigned long long>(env.pool->Stats().resumes),
        static_cast<unsigned long long>(env.pool->Stats().restarts), static_cast<unsigned long long>(env.pool->Stats().headersRead));
}

// With no retained cursor at all (idle limit 0) every read reopens, and the
// handoff still serves the window from one pass per read that needs one.
FIC2_TEST(ArchiveHandoff_NoRetainedCursorsStaysServiceable)
{
    Floar::ArchiveCursorLimits limits;
    limits.maxIdle = 0;
    limits.maxIdlePerReader = 0;
    Env env(L"nocursor", {}, limits);
    FIC2_REQUIRE(env.written);
    auto window = env.Want(0, 9);
    FIC2_CHECK(env.Read(9) == env.Expected(9));
    for (int i = 0; i < 9; ++i)
    {
        FIC2_CHECK(env.Read(i) == env.Expected(i));
    }
    FIC2_CHECK(env.pool->Stats().idle == 0);
    FIC2_CHECK(env.pool->Stats().opens == 1);
    FIC2_CHECK(env.handoff.CurrentStats().hits == 9);
}

// -------------------------------------------------------------------------
// Byte budget, oversize entries, a large demand
// -------------------------------------------------------------------------

// Parking stops at the budget (backpressure: the entry is dropped, never
// waited for) and never takes an entry over maxParkedEntryBytes; the dropped
// and the oversize entries are read again by their own requests, and the
// oversize one is returned whole to its own read. Parked plus the entry in
// hand never exceeds the budget plus one entry.
FIC2_TEST(ArchiveHandoff_ByteBudgetAndOversizeEntries)
{
    constexpr int kOversize = 3;
    ArchiveEntryHandoff::Limits limits;
    limits.maxParkedBytes = 4 * kEntrySize;
    limits.maxParkedEntries = 100;
    limits.maxParkedEntryBytes = kEntrySize;
    const SizeOf sizes = [](int i) { return i == kOversize ? 4 * kEntrySize : kEntrySize; };
    Env env(L"budget", limits, {}, sizes);
    FIC2_REQUIRE(env.written);

    auto window = env.Want(0, 11);
    FIC2_CHECK(env.Read(11) == env.Expected(11));
    auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.droppedOversize == 1);
    FIC2_CHECK(stats.parked == 4);              // 0, 1, 2, 4: the budget is full
    FIC2_CHECK(stats.droppedFull == 6);         // 5..10
    FIC2_CHECK(stats.parkedBytes == 4 * kEntrySize);
    FIC2_CHECK(stats.peakParkedBytes <= limits.maxParkedBytes);
    FIC2_CHECK(stats.peakHeldBytes <= limits.maxParkedBytes + 4 * kEntrySize);

    for (int i = 0; i < 11; ++i)
    {
        FIC2_CHECK(env.Read(i) == env.Expected(i));   // the oversize entry whole, from its own read
    }
    stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.hits == 4);
    FIC2_CHECK(stats.parkedBytes == 0);
    FIC2_CHECK(stats.peakParkedBytes <= limits.maxParkedBytes);
    FIC2_CHECK(stats.peakParkedEntries <= 4);
    std::printf("  parked %llu, dropped full %llu, oversize %llu; peak parked %llu bytes, peak held %llu bytes\n",
        static_cast<unsigned long long>(stats.parked), static_cast<unsigned long long>(stats.droppedFull),
        static_cast<unsigned long long>(stats.droppedOversize), static_cast<unsigned long long>(stats.peakParkedBytes),
        static_cast<unsigned long long>(stats.peakHeldBytes));
}

// A huge demand (every entry of a large folder at once, which the strip never
// registers: see ThumbStripTiles_TenThousandArchiveItemsKeepDemandBounded)
// still parks at most the budget and collects at most maxPassEntries per pass,
// and no more entries than could be parked.
FIC2_TEST(ArchiveHandoff_LargeDemandParksWithinTheBudget)
{
    constexpr int kCount = 10000;
    ArchiveEntryHandoff::Limits limits;
    limits.maxParkedEntries = 1000;
    limits.maxParkedBytes = 50 * 64;   // the scripted entries have 64 bytes
    limits.maxPassEntries = 200;
    ScriptedEnv env(L"large", kCount, L"scripted.7z", limits);
    std::vector<Demand> demands;
    demands.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        demands.push_back(env.Want(i));
    }
    FIC2_CHECK(env.Read(kCount - 1) == ScriptedReader::BytesOf(ScriptedEnv::Name(kCount - 1)));
    const auto passes = env.reader->Passes();
    FIC2_REQUIRE(passes.size() == 1);
    FIC2_CHECK(passes[0].paths.size() == limits.maxPassEntries + 1);   // the nearest 200, then the target
    FIC2_CHECK(passes[0].paths.front() == ScriptedEnv::Name(kCount - 1 - static_cast<int>(limits.maxPassEntries)));
    const auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.parkedEntries == 50);
    FIC2_CHECK(stats.droppedFull == limits.maxPassEntries - 50);

    // Room for 10 more entries: the next pass collects only the nearest 10.
    limits.maxParkedEntries = 60;
    limits.maxParkedBytes = 60 * 64;
    env.handoff.SetLimits(limits);
    FIC2_CHECK(env.Read(kCount - 2) == ScriptedReader::BytesOf(ScriptedEnv::Name(kCount - 2)));
    FIC2_CHECK(env.reader->Passes().back().paths.size() == 10 + 1);
    // Full: a pass is the requested entry alone.
    limits.maxParkedEntries = 50;
    env.handoff.SetLimits(limits);
    FIC2_CHECK(env.Read(kCount - 20) == ScriptedReader::BytesOf(ScriptedEnv::Name(kCount - 20)));
    FIC2_CHECK(env.reader->Passes().back().paths.size() == 1);

    demands.clear();
    FIC2_CHECK(env.handoff.CurrentStats().parkedEntries == 0);
    FIC2_CHECK(env.handoff.CurrentStats().peakParkedBytes <= 60 * 64);
}

// -------------------------------------------------------------------------
// Generations, replaced archives, cancellation (scripted reader)
// -------------------------------------------------------------------------

// A demand withdrawn while a pass runs (its tile recycled, its folder left)
// gets nothing parked, also when the same entry is demanded again by a newer
// request meanwhile: bytes go only to the demand the pass started for.
FIC2_TEST(ArchiveHandoff_WithdrawnDemandsNeverReceiveStaleBytes)
{
    ScriptedEnv env(L"stale", 8);
    std::vector<Demand> demands;
    for (int i = 0; i < 6; ++i)
    {
        demands.push_back(env.Want(i));
    }
    Demand newer;
    env.reader->beforeEntry = [&](size_t order)
    {
        if (order == 0)
        {
            demands[1].Reset();                 // withdrawn
            demands[2].Reset();                 // withdrawn and demanded again (a newer folder)
            newer = env.Want(2);
        }
    };
    FIC2_CHECK(env.Read(5) == ScriptedReader::BytesOf(ScriptedEnv::Name(5)));
    env.reader->beforeEntry = {};
    auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.parked == 3);              // 0, 3, 4
    FIC2_CHECK(stats.droppedStale == 2);        // 1 and 2

    // The newer demand of 2 is read from the archive, never from the old pass.
    const size_t passesBefore = env.reader->Passes().size();
    FIC2_CHECK(env.Read(2) == ScriptedReader::BytesOf(ScriptedEnv::Name(2)));
    FIC2_CHECK(env.reader->Passes().size() == passesBefore + 1);
    FIC2_CHECK(env.handoff.CurrentStats().hits == 0);

    // Navigation: every demand withdrawn releases every parked entry.
    demands.clear();
    newer.Reset();
    stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.parkedEntries == 0);
    FIC2_CHECK(stats.parkedBytes == 0);
    FIC2_CHECK(stats.released == 3);
    FIC2_CHECK(stats.demands == 0);
    // A request of the next folder for entry 0 reads the archive again.
    Demand next = env.Want(0);
    FIC2_CHECK(env.Read(0) == ScriptedReader::BytesOf(ScriptedEnv::Name(0)));
    FIC2_CHECK(env.handoff.CurrentStats().hits == 0);
}

// A stop during a pass returns nothing for the stopped request; entries
// parked before the stop stay with their demands, and the next read (a later
// entry, a parked one, the stopped one again) is served.
FIC2_TEST(ArchiveHandoff_StoppedPassLeavesEverythingServiceable)
{
    ScriptedEnv env(L"stop", 10);
    std::vector<Demand> demands;
    for (int i = 0; i < 8; ++i)
    {
        demands.push_back(env.Want(i));
    }
    std::stop_source stop;
    env.reader->beforeEntry = [&](size_t order)
    {
        if (order == 3)
        {
            stop.request_stop();
        }
    };
    FIC2_CHECK(env.Read(6, stop.get_token()).empty());
    env.reader->beforeEntry = {};
    FIC2_CHECK(env.handoff.CurrentStats().parked == 3);   // 0, 1, 2

    FIC2_CHECK(env.Read(1) == ScriptedReader::BytesOf(ScriptedEnv::Name(1)));   // parked
    FIC2_CHECK(env.handoff.CurrentStats().hits == 1);
    // The stopped request's tile asks again (a new request, a new demand).
    demands[6] = env.Want(6);
    FIC2_CHECK(env.Read(6) == ScriptedReader::BytesOf(ScriptedEnv::Name(6)));
    FIC2_CHECK(env.Read(7) == ScriptedReader::BytesOf(ScriptedEnv::Name(7)));
    FIC2_CHECK(env.Read(0) == ScriptedReader::BytesOf(ScriptedEnv::Name(0)));
    // A pre-stopped read starts nothing.
    std::stop_source stopped;
    stopped.request_stop();
    const size_t passes = env.reader->Passes().size();
    FIC2_CHECK(env.Read(2, stopped.get_token()).empty());
    FIC2_CHECK(env.reader->Passes().size() == passes);
}

// An archive replaced (renamed over) after its entries were parked: the
// cache opens the new file, so the parked bytes of the old one are dropped
// and the entry is read from the replacement.
FIC2_TEST(ArchiveHandoff_ReplacedArchiveDropsParkedBytes)
{
    Env env(L"replace");
    FIC2_REQUIRE(env.written);
    auto window = env.Want(0, 5);
    FIC2_CHECK(env.Read(5) == env.Expected(5));
    FIC2_CHECK(env.handoff.CurrentStats().parkedEntries == 5);

    const auto replacement = env.dir / L"replacement.7z";
    FIC2_REQUIRE(WriteFixture(replacement, 'B', {}, L".dds"));
    // Renaming over the archive works while the cursor is idle (Floar#7).
    FIC2_REQUIRE(MoveFileExW(replacement.c_str(), env.archive.c_str(), MOVEFILE_REPLACE_EXISTING));
    FILETIME later {};
    GetSystemTimeAsFileTime(&later);
    ULARGE_INTEGER value { { later.dwLowDateTime, later.dwHighDateTime } };
    value.QuadPart += 36000000000ull;   // one hour: a different write time for the change check
    later = { value.LowPart, value.HighPart };
    {
        const HANDLE file = CreateFileW(env.archive.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FIC2_REQUIRE(file != INVALID_HANDLE_VALUE);
        FIC2_CHECK(SetFileTime(file, nullptr, nullptr, &later) != FALSE);
        CloseHandle(file);
    }

    FIC2_CHECK(env.Read(2) == env.Expected(2, 'B'));
    const auto stats = env.handoff.CurrentStats();
    FIC2_CHECK(stats.hits == 0);
    FIC2_CHECK(stats.droppedStale == 1);
}

// -------------------------------------------------------------------------
// Main images and unchanged paths
// -------------------------------------------------------------------------

// A main-image read is one entry at High cursor priority: it neither runs nor
// joins a thumbnail pass, also when the entry is a demanded thumbnail too.
// A parked entry is served to it without archive access.
FIC2_TEST(ArchiveHandoff_MainImageReadsAloneAtHighPriority)
{
    ScriptedEnv env(L"main", 10);
    std::vector<Demand> thumbs;
    for (int i = 0; i < 6; ++i)
    {
        thumbs.push_back(env.Want(i));
    }
    Demand main = env.Want(5, Kind::MainImage);
    FIC2_CHECK(env.Read(5) == ScriptedReader::BytesOf(ScriptedEnv::Name(5)));
    auto passes = env.reader->Passes();
    FIC2_REQUIRE(passes.size() == 1);
    FIC2_CHECK(passes[0].paths.size() == 1);
    FIC2_CHECK(passes[0].priority == Floar::ExtractPriority::High);
    FIC2_CHECK(env.handoff.CurrentStats().mainReads == 1);
    FIC2_CHECK(env.handoff.CurrentStats().passes == 0);

    // A main image elsewhere in the window: a thumbnail pass parks it, and the
    // main read takes it.
    Demand main2 = env.Want(2, Kind::MainImage);
    FIC2_CHECK(env.Read(4) == ScriptedReader::BytesOf(ScriptedEnv::Name(4)));   // the thumbnail pass parks 0..3
    FIC2_CHECK(env.Read(2) == ScriptedReader::BytesOf(ScriptedEnv::Name(2)));
    FIC2_CHECK(env.handoff.CurrentStats().hits == 1);
    passes = env.reader->Passes();
    FIC2_CHECK(passes.size() == 2);
    FIC2_CHECK(passes[1].priority == Floar::ExtractPriority::Normal);
}

// Plain files, ZIP and readers with cheap random access (BA2/BSA) read exactly
// as before: one ExtractForward of the requested entry at Normal priority, no
// pass, nothing parked, no cursor; so does an undemanded solid-archive read.
FIC2_TEST(ArchiveHandoff_PlainZipBa2AndUndemandedReadsAreUnchanged)
{
    // Plain file.
    {
        Env env(L"plain");
        const auto plain = env.dir / L"plain.dds";
        std::ofstream(plain, std::ios::binary) << "plain bytes";
        Demand demand = env.handoff.Register(plain.wstring(), Kind::Thumbnail);
        FIC2_CHECK(!demand);
        const auto bytes = env.source.ReadAll(plain.wstring(), {});
        FIC2_CHECK(std::string(bytes.begin(), bytes.end()) == "plain bytes");
        const auto stats = env.handoff.CurrentStats();
        FIC2_CHECK(stats.passes == 0 && stats.parked == 0 && stats.demands == 0);
    }
    // ZIP (no cursor: one scan per read, as before).
    {
        Env env(L"zip");
        const auto zip = env.dir / L"plain.zip";
        std::vector<std::pair<std::wstring, std::string>> files;
        for (int i = 0; i < 6; ++i)
        {
            files.emplace_back(DeepName(i), EntryData(kOther + i, 'z', 4096));
        }
        FIC2_REQUIRE(FloarTest::WriteZip(zip, files, {}));
        std::vector<Demand> demands;
        for (int i = 0; i < 6; ++i)
        {
            demands.push_back(env.handoff.Register(zip.wstring() + L"\\" + DeepName(i), Kind::Thumbnail));
        }
        for (int i = 5; i >= 0; --i)
        {
            FIC2_CHECK(env.source.ReadAll(zip.wstring() + L"\\" + DeepName(i), {}) == Bytes(EntryData(kOther + i, 'z', 4096)));
        }
        const auto stats = env.handoff.CurrentStats();
        FIC2_CHECK(stats.passes == 0);
        FIC2_CHECK(stats.parked == 0);
        FIC2_CHECK(stats.directReads == 6);
        FIC2_CHECK(env.pool->Stats().opens == 0);
    }
    // BA2-like reader: no archive order, no expensive random access.
    {
        ScriptedEnv env(L"ba2", 6, L"textures.ba2");
        env.reader->expensive = false;
        env.reader->format = L"BA2";
        std::vector<Demand> demands;
        for (int i = 0; i < 6; ++i)
        {
            demands.push_back(env.Want(i));
        }
        for (int i = 5; i >= 0; --i)
        {
            FIC2_CHECK(env.Read(i) == ScriptedReader::BytesOf(ScriptedEnv::Name(i)));
        }
        const auto passes = env.reader->Passes();
        FIC2_CHECK(passes.size() == 6);
        for (const auto& pass : passes)
        {
            FIC2_CHECK(pass.paths.size() == 1);
            FIC2_CHECK(pass.priority == Floar::ExtractPriority::Normal);
        }
        FIC2_CHECK(env.handoff.CurrentStats().parked == 0);
        FIC2_CHECK(env.handoff.CurrentStats().directReads == 6);
    }
    // Undemanded reads of a solid 7z: the VFS read, one entry at a time.
    {
        Env env(L"undemanded");
        FIC2_REQUIRE(env.written);
        FIC2_CHECK(env.Read(3) == env.Expected(3));
        FIC2_CHECK(env.Read(1) == env.Expected(1));
        const auto stats = env.handoff.CurrentStats();
        FIC2_CHECK(stats.passes == 0);
        FIC2_CHECK(stats.directReads == 2);
        FIC2_CHECK(env.pool->Stats().opens == 2);   // the backward read restarts, as before
    }
}

// Leaving an archive closes the idle forward cursors; staying in it or
// entering one does not.
FIC2_TEST(ArchiveHandoff_LeavingAnArchiveTrimsIdleCursors)
{
    const auto dir = MakeTempDir(L"leave");
    const auto archive = dir / L"solid.7z";
    FIC2_REQUIRE(WriteFixture(archive, 'a', {}, L".dds"));
    {
        const auto reader = Floar::ArchiveReaderFactory::Open(archive, {}, Floar::ArchiveCursorPool::Shared());
        FIC2_REQUIRE(reader != nullptr);
        FIC2_CHECK(reader->ExtractToMemory(DeepName(0)) == Bytes(EntryData(kOther, 'a', kEntrySize)));
        FIC2_REQUIRE(Floar::ArchiveCursorPool::Shared()->Stats().idle >= 1);

        const Floar::VirtualPath inside(archive, L"Data/Textures");
        const Floar::VirtualPath deeper(archive, L"Data/Textures/Deep");
        const Floar::VirtualPath root(archive);
        const Floar::VirtualPath outside(dir);
        FIC2_CHECK(!TrimArchiveCursorsOnLeave(outside, inside));
        FIC2_CHECK(!TrimArchiveCursorsOnLeave(inside, deeper));
        FIC2_CHECK(!TrimArchiveCursorsOnLeave(deeper, root));
        FIC2_CHECK(Floar::ArchiveCursorPool::Shared()->Stats().idle >= 1);
        FIC2_CHECK(TrimArchiveCursorsOnLeave(deeper, outside));
        FIC2_CHECK(Floar::ArchiveCursorPool::Shared()->Stats().idle == 0);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -------------------------------------------------------------------------
// Through ImageCore (real ImageLoader, bindings as the tiles use them)
// -------------------------------------------------------------------------

namespace
{
    // Extension decoded by SlowDecoderFactory (no built-in decoder claims it).
    constexpr wchar_t kSlowExtension[] = L".fic2s";

    // Decodes kSlowExtension paths to a 1x1 image after `delay`, and checks
    // that the bytes it got are its entry's: the first byte is the archive
    // index the path names (t<NN> -> kOther + NN).
    class SlowDecoderFactory final : public ImageCore::IImageDecoderFactory
    {
    public:
        static SlowDecoderFactory& Instance()
        {
            static std::shared_ptr<SlowDecoderFactory> factory = []
            {
                auto created = std::make_shared<SlowDecoderFactory>();
                ImageCore::DecoderRegistry::Instance().RegisterFactory(created);
                return created;
            }();
            return *factory;
        }

        std::atomic<int> delayMs { 20 };
        std::atomic<int> decodes { 0 };
        std::atomic<int> wrongBytes { 0 };
        std::atomic<int> concurrent { 0 };

        std::wstring_view Id() const override { return L"FICture2Tests.SlowDecoder"; }
        std::wstring_view DisplayName() const override { return L"Slow test decoder"; }
        int Priority() const override { return 1000; }
        std::span<const std::wstring_view> SupportedExtensions() const override
        {
            static const std::wstring_view exts[] = { kSlowExtension };
            return exts;
        }
        bool SupportsPurpose(ImageCore::ImagePurpose) const override { return true; }
        std::unique_ptr<ImageCore::IImageDecoder> Create(ImageCore::ImagePurpose) const override
        {
            return std::make_unique<Decoder>(const_cast<SlowDecoderFactory&>(*this));
        }

    private:
        class Decoder final : public ImageCore::IImageDecoder
        {
        public:
            explicit Decoder(SlowDecoderFactory& owner)
                : m_owner(owner)
            {
            }

            ImageCore::PipelineResult Decode(const ImageCore::ImageRequest& request, IWICImagingFactory*, const ImageCore::DecodeInput& input) override
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(m_owner.delayMs.load()));
                const size_t slash = request.source.find_last_of(L"/\\");
                const bool named = slash != std::wstring::npos && slash + 1 < request.source.size() && request.source[slash + 1] == L't';
                const int deep = named ? _wtoi(request.source.c_str() + slash + 2) : -1;
                if (input.bytes.empty() || deep < 0 || input.bytes[0] != static_cast<uint8_t>(kOther + deep))
                {
                    ++m_owner.wrongBytes;
                }
                ++m_owner.decodes;
                ImageCore::PipelineResult result(S_OK);
                result.image.width = 1;
                result.image.height = 1;
                result.image.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
                result.image.rowPitchBytes = 4;
                result.image.blockBytes = 4;
                result.image.blocks = std::make_shared<std::vector<uint8_t>>(4, static_cast<uint8_t>(0xFF));
                result.imageSize = ImageCore::Size { 1.0f, 1.0f };
                return result;
            }

        private:
            SlowDecoderFactory& m_owner;
        };
    };

    ImageCore::ImageRequest ThumbRequest(const std::wstring& path)
    {
        ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::Thumbnail);
        request.targetSize = ImageCore::Size { 64.0f, 64.0f };
        request.allowGpuCompressedDDS = false;
        return request;
    }

    // The shared handoff (where bindings register), with test limits, its
    // parked bytes cleared and counters reset; restored afterwards.
    struct SharedHandoffScope
    {
        ArchiveEntryHandoff& handoff { ArchiveEntryHandoff::Shared() };
        ArchiveEntryHandoff::Limits previous { handoff.CurrentLimits() };

        explicit SharedHandoffScope(const ArchiveEntryHandoff::Limits& limits)
        {
            handoff.Clear();
            handoff.SetLimits(limits);
            handoff.ResetCounters();
        }

        ~SharedHandoffScope()
        {
            handoff.Clear();
            handoff.SetLimits(previous);
        }
    };

    // ImageCore reads through `source` while in scope.
    struct InstalledSource
    {
        ImageCore::IPathByteSource* previous { ImageCore::GetPathByteSource() };
        explicit InstalledSource(ImageCore::IPathByteSource& source) { ImageCore::SetPathByteSource(&source); }
        ~InstalledSource() { ImageCore::SetPathByteSource(previous); }
    };
}

// A window of thumbnails requested visible-first (reverse archive order) plus
// a main image, through ImageCore, with a one-entry handoff and slow decoding:
// every request completes with its own entry's bytes, nothing deadlocks, and
// afterwards no demand, parked byte or cursor lease is left.
FIC2_TEST(ArchiveHandoff_TinyCacheWithSlowDecodingCompletesThroughImageCore)
{
    ArchiveEntryHandoff::Limits limits;
    limits.maxParkedBytes = kEntrySize;
    limits.maxParkedEntries = 1;
    SharedHandoffScope shared(limits);
    SlowDecoderFactory& decoder = SlowDecoderFactory::Instance();
    decoder.decodes = 0;
    decoder.wrongBytes = 0;
    decoder.delayMs = 20;

    Env env(L"tiny", {}, {}, {}, kSlowExtension);
    FIC2_REQUIRE(env.written);
    FloarPathByteSource source(*env.cache);   // reads through the shared handoff
    InstalledSource installed(source);

    constexpr int kWindow = 24;
    std::vector<std::unique_ptr<ImageAsyncBinding>> thumbs;
    for (int i = kWindow - 1; i >= 0; --i)
    {
        auto binding = std::make_unique<ImageAsyncBinding>();
        const std::wstring path = CommonUtil::NormalizePath(env.Path(i));
        binding->RequestLoad(ThumbRequest(path), path, ImageCore::RequestPriority::Visible);
        thumbs.push_back(std::move(binding));
    }
    ImageAsyncBinding main;
    const std::wstring mainPath = CommonUtil::NormalizePath(env.Path(kWindow + 2));
    ImageCore::ImageRequest full(mainPath, ImageCore::ImagePurpose::FullResolution);
    full.allowGpuCompressedDDS = false;
    main.RequestLoad(full, mainPath);

    size_t applied = 0;
    std::vector<bool> done(thumbs.size(), false);
    bool mainDone = false;
    const bool finished = WaitUntil([&]
    {
        for (size_t i = 0; i < thumbs.size(); ++i)
        {
            if (!done[i] && thumbs[i]->PollAndApply([](ImageAsyncBinding::Payload&&) {}))
            {
                done[i] = true;
                ++applied;
            }
        }
        if (!mainDone && main.PollAndApply([](ImageAsyncBinding::Payload&&) {}))
        {
            mainDone = true;
        }
        return applied == thumbs.size() && mainDone;
    }, 60s);
    const auto stats = shared.handoff.CurrentStats();
    std::printf("  %zu/%zu thumbnails and main %s; passes %llu, hits %llu, parked %llu, dropped full %llu, main reads %llu; opens %llu, resumes %llu\n",
        applied, thumbs.size(), mainDone ? "done" : "pending",
        static_cast<unsigned long long>(stats.passes), static_cast<unsigned long long>(stats.hits),
        static_cast<unsigned long long>(stats.parked), static_cast<unsigned long long>(stats.droppedFull),
        static_cast<unsigned long long>(stats.mainReads),
        static_cast<unsigned long long>(env.pool->Stats().opens), static_cast<unsigned long long>(env.pool->Stats().resumes));
    FIC2_CHECK(finished);
    FIC2_CHECK(decoder.wrongBytes.load() == 0);
    FIC2_CHECK(stats.mainReads == 1);
    FIC2_CHECK(stats.peakParkedBytes <= limits.maxParkedBytes);
    FIC2_CHECK(stats.peakParkedEntries <= 1);
    FIC2_CHECK(stats.demands == 0);   // every completed request withdrew its demand
    FIC2_CHECK(stats.parkedEntries == 0);
    FIC2_CHECK(env.pool->Stats().active == 0);
    FIC2_CHECK(WaitUntil([] { return ImageCore::ImageLoader::Instance().LiveRequestCount() == 0; }));
}

// Navigation through ImageCore: a window's requests are canceled while its
// pass runs (a slow archive read). Their demands are withdrawn at once, so
// whatever the pass still extracts is never parked; the pass stops on the
// read's token, and the next folder's thumbnails (the same archive) complete
// with their own bytes.
FIC2_TEST(ArchiveHandoff_NavigationCancelsThePassAndTheNextFolderLoads)
{
    SharedHandoffScope shared(ArchiveEntryHandoff::Limits {});
    SlowDecoderFactory& decoder = SlowDecoderFactory::Instance();
    decoder.wrongBytes = 0;
    decoder.delayMs = 1;

    Env env(L"navigate", {}, {}, {}, kSlowExtension);
    FIC2_REQUIRE(env.written);
    FloarPathByteSource source(*env.cache);
    InstalledSource installed(source);

    // Slow archive reads: every block read waits while `hold` is set.
    std::atomic<bool> hold { true };
    std::atomic<int> blockReads { 0 };
    Floar::ArchiveReaderFactory::SetLibArchiveReadHookForTesting([&](uint64_t)
    {
        ++blockReads;
        while (hold.load())
        {
            std::this_thread::sleep_for(1ms);
        }
        return false;
    });
    struct HookReset
    {
        ~HookReset() { Floar::ArchiveReaderFactory::SetLibArchiveReadHookForTesting({}); }
    } hookReset;

    // Folder A: entries 20..27, the last first. The reader's scan reads
    // blocks too: it runs before the hook holds.
    hold = false;
    (void)env.cache->Get(env.archive);
    hold = true;
    blockReads = 0;
    std::vector<std::unique_ptr<ImageAsyncBinding>> folderA;
    for (int i = 27; i >= 20; --i)
    {
        auto binding = std::make_unique<ImageAsyncBinding>();
        const std::wstring path = CommonUtil::NormalizePath(env.Path(i));
        binding->RequestLoad(ThumbRequest(path), path, ImageCore::RequestPriority::Visible);
        folderA.push_back(std::move(binding));
    }
    FIC2_REQUIRE(WaitUntil([&] { return blockReads.load() > 0; }));   // the pass is reading
    FIC2_CHECK(shared.handoff.CurrentStats().passes == 1);

    // Navigate: folder A's tiles go away (their requests are canceled).
    folderA.clear();
    FIC2_CHECK(shared.handoff.CurrentStats().demands == 0);
    hold = false;

    // Folder B (other entries of the same archive) completes with its own bytes.
    std::vector<std::unique_ptr<ImageAsyncBinding>> folderB;
    for (int i = 0; i < 6; ++i)
    {
        auto binding = std::make_unique<ImageAsyncBinding>();
        const std::wstring path = CommonUtil::NormalizePath(env.Path(i));
        binding->RequestLoad(ThumbRequest(path), path, ImageCore::RequestPriority::Visible);
        folderB.push_back(std::move(binding));
    }
    size_t applied = 0;
    std::vector<bool> done(folderB.size(), false);
    FIC2_CHECK(WaitUntil([&]
    {
        for (size_t i = 0; i < folderB.size(); ++i)
        {
            if (!done[i] && folderB[i]->PollAndApply([](ImageAsyncBinding::Payload&&) {}))
            {
                done[i] = true;
                ++applied;
            }
        }
        return applied == folderB.size();
    }, 60s));
    FIC2_CHECK(decoder.wrongBytes.load() == 0);
    FIC2_CHECK(WaitUntil([] { return ImageCore::ImageLoader::Instance().LiveRequestCount() == 0; }));
    const auto stats = shared.handoff.CurrentStats();
    FIC2_CHECK(stats.parked == 0);   // folder A's pass parked nothing for its withdrawn demands
    FIC2_CHECK(stats.parkedEntries == 0);
    FIC2_CHECK(stats.demands == 0);
    FIC2_CHECK(env.pool->Stats().active == 0);
}
#endif
