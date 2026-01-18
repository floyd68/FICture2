#pragma once

#include <filesystem>
#include <string>
#include <optional>

// VirtualPath represents a file location that can be either:
// 1. A regular file on disk: D:\textures\image.dds
// 2. A file inside an archive: D:\textures\pack.zip\folder\image.dds
//
// This allows treating archives as virtual directories for seamless navigation.

struct VirtualPath
{
    std::filesystem::path hostPath;      // Physical file path (may be archive or regular file)
    std::wstring archiveInnerPath;       // Path inside archive (empty if not in archive)

    // Constructors
    VirtualPath() = default;
    
    VirtualPath(const std::filesystem::path& path)
        : hostPath(path)
    {
    }

    VirtualPath(const std::filesystem::path& archive, const std::wstring& innerPath)
        : hostPath(archive)
        , archiveInnerPath(innerPath)
    {
    }

    // Check if this path points to a file inside an archive
    bool IsInArchive() const
    {
        return !archiveInnerPath.empty();
    }

    // Check if the host path is an archive file
    bool IsArchiveFile() const;

    // Get the full display path (e.g., "D:\pack.zip\folder\image.dds")
    std::wstring GetDisplayPath() const
    {
        if (IsInArchive())
        {
            return hostPath.wstring() + L"\\" + archiveInnerPath;
        }
        return hostPath.wstring();
    }

    // Get just the filename (without path)
    std::wstring GetFilename() const
    {
        if (IsInArchive())
        {
            auto pos = archiveInnerPath.find_last_of(L"/\\");
            if (pos != std::wstring::npos)
            {
                return archiveInnerPath.substr(pos + 1);
            }
            return archiveInnerPath;
        }
        return hostPath.filename().wstring();
    }

    // Get the extension (e.g., ".dds")
    std::wstring GetExtension() const
    {
        std::wstring filename = GetFilename();
        auto pos = filename.find_last_of(L'.');
        if (pos != std::wstring::npos)
        {
            return filename.substr(pos);
        }
        return L"";
    }

    // Get parent path
    VirtualPath GetParent() const;

    // Check if path exists (either on disk or in archive)
    bool Exists() const;

    // Comparison operators
    bool operator==(const VirtualPath& other) const
    {
        return hostPath == other.hostPath && archiveInnerPath == other.archiveInnerPath;
    }

    bool operator!=(const VirtualPath& other) const
    {
        return !(*this == other);
    }

    // Factory methods
    static VirtualPath FromFilesystem(const std::filesystem::path& path)
    {
        return VirtualPath(path);
    }

    static VirtualPath FromArchive(const std::filesystem::path& archivePath, const std::wstring& innerPath)
    {
        return VirtualPath(archivePath, innerPath);
    }

    // Parse a display path string (e.g., "D:\pack.zip\folder\image.dds")
    // Returns nullopt if the path format is invalid
    static std::optional<VirtualPath> Parse(const std::wstring& displayPath);
};

// Hash function for using VirtualPath in unordered containers
namespace std
{
    template<>
    struct hash<VirtualPath>
    {
        size_t operator()(const VirtualPath& vp) const noexcept
        {
            size_t h1 = hash<wstring>()(vp.hostPath.wstring());
            size_t h2 = hash<wstring>()(vp.archiveInnerPath);
            return h1 ^ (h2 << 1);
        }
    };
}
