#pragma once

// Fakes shared by the archive-read cancellation and app shutdown tests: an
// archive reader / opener pair whose extraction and open block at barriers, a
// recording FloarPathByteSource, and a counting decoder for a test-only
// extension. Every wait is bounded.

#include "FloarPathByteSource.h"

#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/IImageDecoderFactory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ArchiveCancelTest
{
    using namespace std::chrono_literals;

    // Extension decoded by CountingDecoderFactory (no built-in decoder claims it).
    inline constexpr wchar_t kImageExtension[] = L".fic2c";

    inline bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    inline std::filesystem::path MakeTempDir(const std::wstring& tag)
    {
        static std::atomic<unsigned> counter { 0 };
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FICture2ArchiveCancel_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" +
                std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter.fetch_add(1)));
        std::filesystem::create_directories(dir);
        return dir;
    }

    inline std::vector<uint8_t> BytesFor(const std::wstring& name)
    {
        std::vector<uint8_t> bytes(64, 0);
        for (size_t i = 0; i < name.size() && i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<uint8_t>(name[i] & 0xFF);
        }
        return bytes;
    }

    inline void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // Extraction of a "blocking" entry waits at a barrier until Release(), or
    // (cooperative mode) until the read's stop token is requested.
    class GateArchive
    {
    public:
        struct Extraction
        {
            std::wstring entry {};
            bool returnedByStop { false };
            bool delivered { false };
        };

        bool honourStop { true };

        void AddEntry(const std::wstring& name, bool blocking)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.push_back({ name, 64, 64, false, 0 });
            if (blocking)
            {
                m_blocking.insert(name);
            }
        }

        // The reader (one shared instance, as the cache shares it).
        std::shared_ptr<Floar::IArchiveReader> Reader();

        void Release()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_changed.notify_all();
        }

        // Releases one blocking entry (its next extraction runs to completion).
        void ReleaseEntry(const std::wstring& entry)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_releasedEntries.insert(entry);
            m_changed.notify_all();
        }

        bool Released() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_released;
        }

        bool WaitBlocked(const std::wstring& entry, std::chrono::milliseconds timeout = 10s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_waiting.count(entry) != 0; });
        }

        bool WaitDone(const std::wstring& entry, std::chrono::milliseconds timeout = 10s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return Find(entry) != nullptr; });
        }

        // The first finished extraction of `entry` (empty entry name when none).
        Extraction Done(const std::wstring& entry) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Extraction* done = Find(entry);
            return done ? *done : Extraction {};
        }

        std::vector<Extraction> Extractions() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_done;
        }

        size_t InFlight() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_inFlight;
        }

        bool Extract(const std::wstring& entry, std::stop_token stop, std::vector<uint8_t>& out)
        {
            // Registered before taking the lock: it runs inline if stop is already requested.
            std::stop_callback wake(stop, [this] { std::lock_guard<std::mutex> lock(m_mutex); m_changed.notify_all(); });
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_inFlight;
            bool stopped = false;
            if (m_blocking.count(entry) != 0)
            {
                m_waiting.insert(entry);
                m_changed.notify_all();
                const auto released = [&] { return m_released || m_releasedEntries.count(entry) != 0; };
                m_changed.wait(lock, [&] { return released() || (honourStop && stop.stop_requested()); });
                m_waiting.erase(entry);
                stopped = !released();
            }
            if (stop.stop_requested())
            {
                stopped = true;
            }
            if (!stopped)
            {
                out = BytesFor(entry);
            }
            m_done.push_back({ entry, stopped, !stopped });
            --m_inFlight;
            m_changed.notify_all();
            return !stopped;
        }

        const std::vector<Floar::ArchiveEntry>& Entries() const { return m_entries; }

    private:
        const Extraction* Find(const std::wstring& entry) const
        {
            for (const auto& done : m_done)
            {
                if (done.entry == entry)
                {
                    return &done;
                }
            }
            return nullptr;
        }

        mutable std::mutex m_mutex {};
        std::condition_variable m_changed {};
        std::vector<Floar::ArchiveEntry> m_entries {};
        std::set<std::wstring> m_blocking {};
        std::set<std::wstring> m_waiting {};
        std::set<std::wstring> m_releasedEntries {};
        std::vector<Extraction> m_done {};
        size_t m_inFlight { 0 };
        bool m_released { false };
        std::shared_ptr<Floar::IArchiveReader> m_reader {};
    };

    class GateReader final : public Floar::IArchiveReader
    {
    public:
        explicit GateReader(GateArchive& archive)
            : m_archive(archive)
        {
        }

        const std::vector<Floar::ArchiveEntry>& Entries() const override { return m_archive.Entries(); }

        std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override
        {
            std::vector<uint8_t> out;
            (void)m_archive.Extract(entryPath, std::stop_token {}, out);
            return out;
        }

        bool ExtractBatch(const std::vector<std::wstring>& entryPaths, std::stop_token stop, const BatchSink& onEntry) override
        {
            for (const auto& path : entryPaths)
            {
                std::vector<uint8_t> out;
                if (!m_archive.Extract(path, stop, out))
                {
                    return false;
                }
                onEntry(path, std::move(out));
            }
            return true;
        }

        bool HasEntry(const std::wstring&) override { return true; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return L"gate"; }

    private:
        GateArchive& m_archive;
    };

    inline std::shared_ptr<Floar::IArchiveReader> GateArchive::Reader()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_reader)
        {
            m_reader = std::make_shared<GateReader>(*this);
        }
        return m_reader;
    }

    // Opener for an ArchiveReaderCache: returns `archive`'s reader. With
    // blockOpen, every open waits at a barrier until Release(), or (cooperative)
    // until the operation's stop token is requested; a stopped open returns null.
    class GateOpener
    {
    public:
        bool blockOpen { false };
        bool honourStop { true };
        // Runs on the open worker once stop was observed, before returning.
        std::function<void()> afterStop {};

        explicit GateOpener(GateArchive& archive)
            : m_archive(archive)
        {
        }

        Floar::ArchiveReaderCache::Opener Make()
        {
            return [this](const std::filesystem::path&, std::stop_token stop) -> std::shared_ptr<Floar::IArchiveReader>
            {
                std::stop_callback wake(stop, [this] { std::lock_guard<std::mutex> lock(m_mutex); m_changed.notify_all(); });
                bool stopped = false;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    ++m_entered;
                    ++m_inFlight;
                    m_changed.notify_all();
                    if (blockOpen)
                    {
                        m_changed.wait(lock, [&] { return m_released || (honourStop && stop.stop_requested()); });
                    }
                    stopped = stop.stop_requested();
                    if (stopped)
                    {
                        ++m_stopSeen;
                    }
                }
                if (stopped && afterStop)
                {
                    afterStop();
                }
                std::shared_ptr<Floar::IArchiveReader> reader = stopped ? nullptr : m_archive.Reader();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    --m_inFlight;
                    m_changed.notify_all();
                }
                return reader;
            };
        }

        void Release()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_changed.notify_all();
        }

        bool WaitEntered(size_t count, std::chrono::milliseconds timeout = 10s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_entered >= count; });
        }

        size_t StopSeen() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_stopSeen;
        }

        size_t InFlight() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_inFlight;
        }

    private:
        GateArchive& m_archive;
        mutable std::mutex m_mutex {};
        std::condition_variable m_changed {};
        size_t m_entered { 0 };
        size_t m_inFlight { 0 };
        size_t m_stopSeen { 0 };
        bool m_released { false };
    };

    // FloarPathByteSource that records each stop-aware read, in order.
    class RecordingSource final : public FloarPathByteSource
    {
    public:
        struct Event
        {
            enum class Kind { Start, Return } kind { Kind::Start };
            std::wstring path {};
            size_t bytes { 0 };
            bool stopRequested { false };
        };

        explicit RecordingSource(Floar::ArchiveReaderCache& cache)
            : FloarPathByteSource(cache)
        {
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override
        {
            Record({ Event::Kind::Start, path, 0, stop.stop_requested() }, +1);
            std::vector<uint8_t> bytes = FloarPathByteSource::ReadAll(path, stop);
            Record({ Event::Kind::Return, path, bytes.size(), stop.stop_requested() }, -1);
            return bytes;
        }
        using FloarPathByteSource::ReadAll;

        std::vector<Event> Events() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_events;
        }

        size_t InFlight() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_inFlight;
        }

        size_t Count(Event::Kind kind, const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<size_t>(std::count_if(m_events.begin(), m_events.end(),
                [&](const Event& e) { return e.kind == kind && e.path == path; }));
        }

        // Index of the first event of `kind` for `path`, or SIZE_MAX.
        size_t IndexOf(Event::Kind kind, const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = 0; i < m_events.size(); ++i)
            {
                if (m_events[i].kind == kind && m_events[i].path == path)
                {
                    return i;
                }
            }
            return SIZE_MAX;
        }

        // The first finished read of `path` (empty path when none).
        Event Returned(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& e : m_events)
            {
                if (e.kind == Event::Kind::Return && e.path == path)
                {
                    return e;
                }
            }
            return Event {};
        }

    private:
        void Record(Event event, int delta) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(std::move(event));
            m_inFlight = static_cast<size_t>(static_cast<long long>(m_inFlight) + delta);
        }

        mutable std::mutex m_mutex {};
        mutable std::vector<Event> m_events {};
        mutable size_t m_inFlight { 0 };
    };

    // Decodes kImageExtension paths to a 1x1 BGRA image and counts decodes per path.
    class CountingDecoderFactory final : public ImageCore::IImageDecoderFactory
    {
    public:
        static CountingDecoderFactory& Instance()
        {
            static std::shared_ptr<CountingDecoderFactory> factory = []
            {
                auto created = std::make_shared<CountingDecoderFactory>();
                ImageCore::DecoderRegistry::Instance().RegisterFactory(created);
                return created;
            }();
            return *factory;
        }

        size_t Decodes(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_decodes.find(path);
            return it == m_decodes.end() ? 0 : it->second;
        }

        size_t TotalDecodes() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t total = 0;
            for (const auto& entry : m_decodes)
            {
                total += entry.second;
            }
            return total;
        }

        std::wstring_view Id() const override { return L"FICture2Tests.CountingDecoder"; }
        std::wstring_view DisplayName() const override { return L"Counting test decoder"; }
        int Priority() const override { return 1000; }
        std::span<const std::wstring_view> SupportedExtensions() const override
        {
            static const std::wstring_view exts[] = { kImageExtension };
            return exts;
        }
        bool SupportsPurpose(ImageCore::ImagePurpose) const override { return true; }
        std::unique_ptr<ImageCore::IImageDecoder> Create(ImageCore::ImagePurpose) const override
        {
            return std::make_unique<Decoder>(const_cast<CountingDecoderFactory&>(*this));
        }

    private:
        class Decoder final : public ImageCore::IImageDecoder
        {
        public:
            explicit Decoder(CountingDecoderFactory& owner)
                : m_owner(owner)
            {
            }

            ImageCore::PipelineResult Decode(const ImageCore::ImageRequest& request, const ImageCore::DecodeInput&) override
            {
                {
                    std::lock_guard<std::mutex> lock(m_owner.m_mutex);
                    ++m_owner.m_decodes[request.source];
                }
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
            CountingDecoderFactory& m_owner;
        };

        mutable std::mutex m_mutex {};
        std::map<std::wstring, size_t> m_decodes {};
    };
}
