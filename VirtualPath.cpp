#include "VirtualPath.h"
#include "ArchiveReader.h"
#include <algorithm>
#include <cwctype>

namespace
{
    std::wstring ToLower(const std::wstring& str)
    {
        std::wstring result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return result;
    }
}

bool VirtualPath::IsArchiveFile() const
{
    if (IsInArchive())
    {
        return false; // A path inside an archive is not itself an archive
    }
    return ArchiveReaderFactory::IsArchiveFile(hostPath);
}

VirtualPath VirtualPath::GetParent() const
{
    if (IsInArchive())
    {
        // Find parent within archive
        auto pos = archiveInnerPath.find_last_of(L"/\\");
        if (pos != std::wstring::npos && pos > 0)
        {
            // Has parent directory inside archive
            return VirtualPath(hostPath, archiveInnerPath.substr(0, pos));
        }
        else
        {
            // At root of archive, parent is the archive itself
            return VirtualPath(hostPath);
        }
    }
    else
    {
        // Regular filesystem path
        if (hostPath.has_parent_path())
        {
            return VirtualPath(hostPath.parent_path());
        }
        return *this; // Already at root
    }
}

bool VirtualPath::Exists() const
{
    if (IsInArchive())
    {
        // Check if file exists in archive
        auto reader = ArchiveReaderFactory::Open(hostPath);
        if (!reader)
        {
            return false;
        }
        return reader->HasEntry(archiveInnerPath);
    }
    else
    {
        // Check regular filesystem
        return std::filesystem::exists(hostPath);
    }
}

std::optional<VirtualPath> VirtualPath::Parse(const std::wstring& displayPath)
{
    if (displayPath.empty())
    {
        return std::nullopt;
    }

    // Try to find an archive file in the path
    // Look for extensions like .zip, .7z, .rar in the path
    std::wstring lowerPath = ToLower(displayPath);
    
    std::vector<std::wstring> archiveExts = ArchiveReaderFactory::GetSupportedExtensions();
    
    for (const auto& ext : archiveExts)
    {
        size_t pos = lowerPath.find(ext);
        while (pos != std::wstring::npos)
        {
            // Check if this is followed by a path separator
            size_t afterExt = pos + ext.length();
            if (afterExt < displayPath.length() && 
                (displayPath[afterExt] == L'\\' || displayPath[afterExt] == L'/'))
            {
                // Found archive boundary
                std::wstring archivePart = displayPath.substr(0, afterExt);
                std::wstring innerPart = displayPath.substr(afterExt + 1);
                
                // Normalize inner path separators to forward slashes
                std::replace(innerPart.begin(), innerPart.end(), L'\\', L'/');
                
                return VirtualPath(archivePart, innerPart);
            }
            
            // Look for next occurrence
            pos = lowerPath.find(ext, pos + 1);
        }
    }

    // No archive found, treat as regular filesystem path
    return VirtualPath(displayPath);
}
