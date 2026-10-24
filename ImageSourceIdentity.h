#pragma once

// ImageSourceIdentity: what a decoded image was made from, so cached GPU content
// can be recognized as stale when the file at the same path changes.
//   - disk file: its size and last-write time;
//   - archive entry: the host archive's size and last-write time (an archive
//     rewrite invalidates every cached entry from it).
// An identity that cannot be read (missing file) is "unknown" and never equals
// another identity, so it never validates cached content.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "VirtualPath.h"

#include <cstdint>
#include <string>

struct ImageSourceIdentity
{
    uint64_t size { 0 };
    uint64_t writeTime { 0 };
    bool known { false };

    bool operator==(const ImageSourceIdentity& other) const
    {
        return known && other.known && size == other.size && writeTime == other.writeTime;
    }

    bool operator!=(const ImageSourceIdentity& other) const
    {
        return !(*this == other);
    }

    static ImageSourceIdentity ForFile(const std::wstring& filePath)
    {
        ImageSourceIdentity identity {};
        WIN32_FILE_ATTRIBUTE_DATA data {};
        if (filePath.empty() || !GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return identity;
        }
        identity.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        identity.writeTime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        identity.known = true;
        return identity;
    }

    // Identity for a display path (disk file or "archive\entry" virtual path).
    static ImageSourceIdentity ForPath(const std::wstring& displayPath)
    {
        if (const auto vp = Floar::VirtualPath::Parse(displayPath); vp && vp->IsInArchive())
        {
            return ForFile(vp->hostPath.wstring());
        }
        return ForFile(displayPath);
    }
};
