#include "TestHarness.h"

#include "FloarPathByteSource.h"
#include "ImageCore/IPathByteSource.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// FloarPathByteSource::ClassifyPath (ImageCore's I/O lanes, floyd68/ImageCore#15,
// FICture2 #55). The filesystem queries are faked and counted; no file is
// touched except in the System() smoke test, which uses a temp file.

namespace
{
    using ImageCore::IoLane;
    using ImageCore::IoLaneFor;
    using ImageCore::PathSourceKind;

    // Drive types by letter and attributes by path; anything else fails
    // (DRIVE_NO_ROOT_DIR, INVALID_FILE_ATTRIBUTES). Records every call.
    class FakeQueries final : public PathClassificationQueries
    {
    public:
        std::map<wchar_t, std::uint32_t> driveTypes;
        std::map<std::wstring, std::uint32_t> attributes;

        std::uint32_t DriveType(const std::wstring& root) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_driveCalls[root];
            auto it = root.empty() ? driveTypes.end() : driveTypes.find(root[0]);
            return it != driveTypes.end() ? it->second : static_cast<std::uint32_t>(DRIVE_NO_ROOT_DIR);
        }

        std::uint32_t FileAttributes(const std::wstring& path) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_attributeCalls.push_back(path);
            auto it = attributes.find(path);
            return it != attributes.end() ? it->second : INVALID_FILE_ATTRIBUTES;
        }

        size_t DriveCalls(const std::wstring& root) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_driveCalls.find(root);
            return it == m_driveCalls.end() ? 0 : it->second;
        }

        size_t TotalDriveCalls() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t total = 0;
            for (const auto& [root, calls] : m_driveCalls)
            {
                total += calls;
            }
            return total;
        }

        size_t AttributeCalls(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<size_t>(std::count(m_attributeCalls.begin(), m_attributeCalls.end(), path));
        }

        size_t TotalAttributeCalls() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_attributeCalls.size();
        }

    private:
        mutable std::mutex m_mutex;
        mutable std::map<std::wstring, size_t> m_driveCalls;
        mutable std::vector<std::wstring> m_attributeCalls;
    };

    // C: fixed, Z: a mapped network drive, R: removable; nothing else exists.
    void Fill(FakeQueries& queries)
    {
        queries.driveTypes[L'C'] = DRIVE_FIXED;
        queries.driveTypes[L'Z'] = DRIVE_REMOTE;
        queries.driveTypes[L'R'] = DRIVE_REMOVABLE;
        queries.attributes[L"C:\\pics\\plain.png"] = FILE_ATTRIBUTE_ARCHIVE;
        queries.attributes[L"\\\\?\\C:\\pics\\plain.png"] = FILE_ATTRIBUTE_ARCHIVE;
        queries.attributes[L"C:\\OneDrive\\online.png"] = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
        queries.attributes[L"C:\\OneDrive\\offline.png"] = FILE_ATTRIBUTE_OFFLINE;
        queries.attributes[L"C:\\OneDrive\\pinned.png"] = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_PINNED;
    }
}

FIC2_TEST(PathClassification_MapsEachPathToItsLane)
{
    FakeQueries queries;
    Fill(queries);
    const FloarPathByteSource source(queries);

    struct Case
    {
        const wchar_t* path;
        PathSourceKind kind;
        IoLane lane;
    };
    const Case cases[] {
        // A local plain file.
        { L"C:\\pics\\plain.png", PathSourceKind::Local, IoLane::Local },
        { L"\\\\?\\C:\\pics\\plain.png", PathSourceKind::Local, IoLane::Local },
        // An archive entry, and a folder whose name ends in an archive
        // extension (lexical, as Floar's reads treat it).
        { L"C:\\pics\\pack.zip\\inner\\a.dds", PathSourceKind::Archive, IoLane::Archive },
        { L"C:\\mods\\Textures.7z\\b.png", PathSourceKind::Archive, IoLane::Archive },
        { L"\\\\server\\share\\pack.zip\\c.png", PathSourceKind::Archive, IoLane::Archive },
        // UNC paths (lexical).
        { L"\\\\server\\share\\d.png", PathSourceKind::Remote, IoLane::Remote },
        { L"\\\\?\\UNC\\server\\share\\e.png", PathSourceKind::Remote, IoLane::Remote },
        // A mapped remote drive (faked DRIVE_REMOTE).
        { L"Z:\\photos\\f.png", PathSourceKind::Remote, IoLane::Remote },
        // Local cloud placeholders (faked attributes); a pinned file is local.
        { L"C:\\OneDrive\\online.png", PathSourceKind::Remote, IoLane::Remote },
        { L"C:\\OneDrive\\offline.png", PathSourceKind::Remote, IoLane::Remote },
        { L"C:\\OneDrive\\pinned.png", PathSourceKind::Local, IoLane::Local },
        // Unparseable paths and failing queries: the conservative default.
        { L"", PathSourceKind::Unknown, IoLane::Local },
        { L"relative\\g.png", PathSourceKind::Unknown, IoLane::Local },
        { L"C:\\pics\\missing.png", PathSourceKind::Unknown, IoLane::Local },   // attribute query fails
        { L"Q:\\h.png", PathSourceKind::Unknown, IoLane::Local },               // drive type query fails
        { L"R:\\i.png", PathSourceKind::Unknown, IoLane::Local },               // removable: not queried further
    };
    for (const Case& c : cases)
    {
        const PathSourceKind kind = source.ClassifyPath(c.path);
        if (kind != c.kind || IoLaneFor(kind) != c.lane)
        {
            std::fwprintf(stderr, L"  %ls: kind %d, expected %d\n", c.path, static_cast<int>(kind), static_cast<int>(c.kind));
        }
        FIC2_CHECK(kind == c.kind);
        FIC2_CHECK(IoLaneFor(kind) == c.lane);
    }

    // Archive entries are classified lexically: no drive or attribute query.
    FIC2_CHECK(queries.AttributeCalls(L"C:\\pics\\pack.zip\\inner\\a.dds") == 0);
    FIC2_CHECK(queries.AttributeCalls(L"C:\\mods\\Textures.7z\\b.png") == 0);
    // Attribute queries only on the fixed drive.
    FIC2_CHECK(queries.AttributeCalls(L"R:\\i.png") == 0);
    FIC2_CHECK(queries.AttributeCalls(L"Q:\\h.png") == 0);
}

// The host classifier behind ClassifyPath tells a cloud placeholder (which
// ImageCore schedules as Remote) apart from a remote path: the shell thumbnail
// routing of online-only files (#59) uses the same detection.
FIC2_TEST(PathClassification_HostClassifierSeparatesCloudPlaceholders)
{
    FakeQueries queries;
    Fill(queries);
    const HostPathClassifier classifier(&queries);

    FIC2_CHECK(classifier.Classify(L"C:\\OneDrive\\online.png") == HostPathKind::CloudPlaceholder);
    FIC2_CHECK(classifier.Classify(L"C:\\OneDrive\\offline.png") == HostPathKind::CloudPlaceholder);
    FIC2_CHECK(classifier.Classify(L"C:\\OneDrive\\pinned.png") == HostPathKind::Local);
    FIC2_CHECK(classifier.Classify(L"C:\\pics\\plain.png") == HostPathKind::Local);
    FIC2_CHECK(classifier.Classify(L"Z:\\photos\\f.png") == HostPathKind::Remote);
    FIC2_CHECK(classifier.Classify(L"\\\\server\\share\\d.png") == HostPathKind::Remote);
    FIC2_CHECK(classifier.Classify(L"C:\\pics\\pack.zip\\a.png") == HostPathKind::Archive);
    FIC2_CHECK(classifier.Classify(L"C:\\pics\\missing.png") == HostPathKind::Unknown);
    FIC2_CHECK(classifier.Classify(L"") == HostPathKind::Unknown);
}

FIC2_TEST(PathClassification_AvoidsRemoteMetadataQueries)
{
    // UNC and remote-drive paths never reach the attribute query, UNC paths
    // never reach the drive-type query, and the drive type is queried at most
    // once per drive letter, also from concurrent callers. (This checks the
    // call path, not a latency bound.)
    FakeQueries queries;
    Fill(queries);
    const FloarPathByteSource source(queries);
    const std::vector<std::wstring> remote {
        L"\\\\server\\share\\a.png", L"\\\\?\\UNC\\server\\share\\b.png", L"Z:\\c.png", L"z:\\d.png", L"Z:\\e.png" };

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&]
        {
            for (int round = 0; round < 50; ++round)
            {
                for (const auto& path : remote)
                {
                    (void)source.ClassifyPath(path);
                }
                (void)source.ClassifyPath(L"C:\\pics\\plain.png");
                (void)source.ClassifyPath(L"c:\\pics\\other.png");
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    for (const auto& path : remote)
    {
        FIC2_CHECK(queries.AttributeCalls(path) == 0);
        FIC2_CHECK(source.ClassifyPath(path) == PathSourceKind::Remote);
    }
    FIC2_CHECK(queries.DriveCalls(L"Z:\\") == 1);
    FIC2_CHECK(queries.DriveCalls(L"C:\\") == 1);
    FIC2_CHECK(queries.TotalDriveCalls() == 2);   // none for the UNC paths
    // The fixed-drive files were queried (once per classification).
    FIC2_CHECK(queries.AttributeCalls(L"C:\\pics\\plain.png") == 4 * 50);
    FIC2_CHECK(queries.TotalAttributeCalls() == 4 * 50 * 2);
}

FIC2_TEST(PathClassification_SystemQueriesClassifyALocalFile)
{
    // The real queries on a temp file: Local when its drive is fixed (the
    // usual case); a remote temp drive would make it Remote.
    wchar_t tempPath[MAX_PATH] {};
    GetTempPathW(MAX_PATH, tempPath);
    const std::filesystem::path file = std::filesystem::path(tempPath) /
        (L"FICture2PathClassification_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    {
        std::ofstream out(file, std::ios::binary);
        out << "x";
    }
    const FloarPathByteSource source;
    const PathSourceKind kind = source.ClassifyPath(file.wstring());
    const std::wstring root { file.wstring()[0], L':', L'\\' };
    const UINT driveType = GetDriveTypeW(root.c_str());
    if (driveType == DRIVE_FIXED)
    {
        FIC2_CHECK(kind == PathSourceKind::Local);
    }
    else if (driveType == DRIVE_REMOTE)
    {
        FIC2_CHECK(kind == PathSourceKind::Remote);
    }
    FIC2_CHECK(source.ClassifyPath((file.parent_path() / L"pack.zip" / L"a.png").wstring()) == PathSourceKind::Archive);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
