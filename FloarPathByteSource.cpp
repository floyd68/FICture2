#include "FloarPathByteSource.h"

#include "ArchiveEntryHandoff.h"
#include "ArchiveReaderCache.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

#include <windows.h>

#include <cwctype>

namespace
{
    class SystemPathClassificationQueries final : public PathClassificationQueries
    {
    public:
        std::uint32_t DriveType(const std::wstring& root) const override
        {
            return GetDriveTypeW(root.c_str());
        }

        std::uint32_t FileAttributes(const std::wstring& path) const override
        {
            return GetFileAttributesW(path.c_str());
        }
    };

    // "\\server\share\..." or "\\?\UNC\server\...", lexically.
    bool IsUncPath(const std::wstring& path)
    {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        {
            return true;
        }
        return path.size() > 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/') &&
            path.rfind(L"\\\\?\\", 0) != 0 && path.rfind(L"\\\\.\\", 0) != 0;
    }

    // Upper-case drive letter of "C:..." or "\\?\C:...", else 0.
    wchar_t DriveLetterOf(const std::wstring& path)
    {
        const size_t start = (path.rfind(L"\\\\?\\", 0) == 0) ? 4 : 0;
        if (path.size() >= start + 2 && path[start + 1] == L':')
        {
            const wchar_t letter = static_cast<wchar_t>(std::towupper(path[start]));
            if (letter >= L'A' && letter <= L'Z')
            {
                return letter;
            }
        }
        return 0;
    }
}

const PathClassificationQueries& PathClassificationQueries::System()
{
    static const SystemPathClassificationQueries s_queries;
    return s_queries;
}

FloarPathByteSource::FloarPathByteSource(Floar::ArchiveReaderCache& cache)
    : m_cache(&cache)
{
}

FloarPathByteSource::FloarPathByteSource(Floar::ArchiveReaderCache& cache, ArchiveEntryHandoff& handoff)
    : m_cache(&cache)
    , m_handoff(&handoff)
{
}

FloarPathByteSource::FloarPathByteSource(const PathClassificationQueries& queries)
    : m_classifier(&queries)
{
}

Floar::ArchiveReaderCache& FloarPathByteSource::Cache() const
{
    return m_cache ? *m_cache : Floar::ArchiveReaderCache::Shared();
}

ArchiveEntryHandoff& FloarPathByteSource::Handoff() const
{
    return m_handoff ? *m_handoff : ArchiveEntryHandoff::Shared();
}

const PathClassificationQueries& HostPathClassifier::Queries() const
{
    return m_queries ? *m_queries : PathClassificationQueries::System();
}

std::wstring FloarPathByteSource::ResolveHostPath(const std::wstring& path) const
{
    auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
    {
        return path;
    }

    return vpath->hostPath.wstring();
}

std::vector<uint8_t> FloarPathByteSource::ReadAll(const std::wstring& path) const
{
    return ReadAll(path, std::stop_token {});
}

std::vector<uint8_t> FloarPathByteSource::ReadAll(const std::wstring& path, std::stop_token stop) const
{
    if (stop.stop_requested())
    {
        return {};
    }

    auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
    {
        return {};
    }

    return Handoff().Read(*vpath, stop, Cache());
}

std::uint32_t HostPathClassifier::DriveTypeOf(wchar_t letter) const
{
    std::lock_guard<std::mutex> lock(m_driveMutex);
    std::optional<std::uint32_t>& cached = m_driveTypes[static_cast<size_t>(letter - L'A')];
    if (!cached)
    {
        const std::wstring root { letter, L':', L'\\' };
        cached = Queries().DriveType(root);
    }
    return *cached;
}

HostPathKind HostPathClassifier::Classify(const std::wstring& path) const
{
    using Kind = HostPathKind;

    // Lexical checks first: no filesystem access for archives and UNC paths.
    const auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
    {
        return Kind::Unknown;
    }
    if (vpath->IsInArchive())
    {
        return Kind::Archive;
    }
    if (IsUncPath(path))
    {
        return Kind::Remote;
    }

    const wchar_t letter = DriveLetterOf(path);
    if (letter == 0)
    {
        return Kind::Unknown;
    }
    const std::uint32_t driveType = DriveTypeOf(letter);
    if (driveType == DRIVE_REMOTE)
    {
        return Kind::Remote;
    }
    if (driveType != DRIVE_FIXED)
    {
        return Kind::Unknown;
    }

    // A local metadata query on a fixed drive (it does not hydrate a cloud file).
    const std::uint32_t attributes = Queries().FileAttributes(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return Kind::Unknown;
    }
    if (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE))
    {
        return Kind::CloudPlaceholder;
    }
    return Kind::Local;
}

ImageCore::PathSourceKind FloarPathByteSource::ClassifyPath(const std::wstring& path) const
{
    using Kind = ImageCore::PathSourceKind;
    switch (m_classifier.Classify(path))
    {
    case HostPathKind::Local:
        return Kind::Local;
    case HostPathKind::Archive:
        return Kind::Archive;
    case HostPathKind::Remote:
    case HostPathKind::CloudPlaceholder:
        return Kind::Remote;
    case HostPathKind::Unknown:
        break;
    }
    return Kind::Unknown;
}
