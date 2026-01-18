#pragma once

#include "VirtualPath.h"
#include <vector>
#include <span>

// VirtualFileSystem provides unified access to files on disk and inside archives
// It abstracts away the difference between regular files and archived files

struct VirtualFileEntry
{
    VirtualPath path;
    bool isDirectory;
    uint64_t size;
    time_t modTime;

    VirtualFileEntry() = default;

    VirtualFileEntry(const VirtualPath& p, bool isDir, uint64_t sz = 0, time_t mt = 0)
        : path(p)
        , isDirectory(isDir)
        , size(sz)
        , modTime(mt)
    {
    }
};

class VirtualFileSystem
{
public:
    // List all entries in a directory (or archive root)
    // For regular directories: lists files and subdirectories
    // For archives: lists root-level entries
    // For paths inside archives: lists entries in that subdirectory
    static std::vector<VirtualFileEntry> ListDirectory(const VirtualPath& dirPath);

    // Read a file's contents into memory
    // Works for both regular files and files inside archives
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath);

    // Check if a file is an image based on extension
    static bool IsImageFile(const std::wstring& filename);
    static bool IsImageFile(const VirtualPath& path);

    // Get all supported image extensions
    static std::vector<std::wstring> GetImageExtensions();

    // Filter a list of entries to only include images and directories/archives
    static std::vector<VirtualFileEntry> FilterImageEntries(const std::vector<VirtualFileEntry>& entries);

    // Check if path is a directory (or archive, which acts like a directory)
    static bool IsDirectory(const VirtualPath& path);

    // Get all image files recursively from a directory or archive
    static std::vector<VirtualPath> GetAllImages(const VirtualPath& rootPath, bool recursive = false);

private:
    // Helper: List files in a regular filesystem directory
    static std::vector<VirtualFileEntry> ListFilesystemDirectory(const std::filesystem::path& dirPath);

    // Helper: List entries in an archive
    static std::vector<VirtualFileEntry> ListArchiveRoot(const std::filesystem::path& archivePath);

    // Helper: List entries in a subdirectory within an archive
    static std::vector<VirtualFileEntry> ListArchiveSubdirectory(
        const std::filesystem::path& archivePath,
        const std::wstring& innerPath);

    // Helper: Normalize path separators
    static std::wstring NormalizePath(const std::wstring& path);
};
