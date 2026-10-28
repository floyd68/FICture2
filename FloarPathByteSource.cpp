#include "FloarPathByteSource.h"

#include "ArchiveEntryHandoff.h"
#include "ArchiveReaderCache.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

FloarPathByteSource::FloarPathByteSource(Floar::ArchiveReaderCache& cache)
    : m_cache(&cache)
{
}

FloarPathByteSource::FloarPathByteSource(Floar::ArchiveReaderCache& cache, ArchiveEntryHandoff& handoff)
    : m_cache(&cache)
    , m_handoff(&handoff)
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
