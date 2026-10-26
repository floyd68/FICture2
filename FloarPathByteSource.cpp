#include "FloarPathByteSource.h"

#include "ArchiveReaderCache.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

FloarPathByteSource::FloarPathByteSource(Floar::ArchiveReaderCache& cache)
    : m_cache(&cache)
{
}

Floar::ArchiveReaderCache& FloarPathByteSource::Cache() const
{
    return m_cache ? *m_cache : Floar::ArchiveReaderCache::Shared();
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

    return Floar::VirtualFileSystem::ReadFile(*vpath, stop, Cache());
}
