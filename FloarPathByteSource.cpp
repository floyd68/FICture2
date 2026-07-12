#include "FloarPathByteSource.h"

#include "VirtualPath.h"
#include "VirtualFileSystem.h"

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
    auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
    {
        return {};
    }

    return Floar::VirtualFileSystem::ReadFile(*vpath);
}
