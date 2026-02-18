#include "VirtualFileSystem.h"
#include "ArchiveReader.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_set>
#include <Windows.h>

namespace
{
    std::wstring ToLower(const std::wstring& str)
    {
        std::wstring result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return result;
    }

    bool StartsWith(const std::wstring& str, const std::wstring& prefix)
    {
        if (str.length() < prefix.length())
            return false;
        return str.compare(0, prefix.length(), prefix) == 0;
    }

    std::vector<std::wstring> GetSupportedImageExtensions()
    {
        return ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
    }
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListDirectory(const VirtualPath& dirPath)
{
    if (dirPath.IsInArchive())
    {
        // List subdirectory within archive
        return ListArchiveSubdirectory(dirPath.hostPath, dirPath.archiveInnerPath);
    }
    else if (dirPath.IsArchiveFile())
    {
        // List root of archive
        return ListArchiveRoot(dirPath.hostPath);
    }
    else
    {
        // List regular filesystem directory
        return ListFilesystemDirectory(dirPath.hostPath);
    }
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(const VirtualPath& filePath)
{
    if (filePath.IsInArchive())
    {
        // Read from archive
        auto reader = ArchiveReaderFactory::Open(filePath.hostPath);
        if (!reader)
        {
            OutputDebugStringW(L"[VFS] Failed to open archive reader.\n");
            return {};
        }
        
        auto data = reader->ExtractToMemory(filePath.archiveInnerPath);
        if (data.empty())
        {
            OutputDebugStringW(L"[VFS] Archive entry read returned empty data.\n");
        }
        return data;
    }
    else
    {
        // Read regular file
        std::ifstream file(filePath.hostPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return {};
        }

        auto size = file.tellg();
        if (size <= 0 || size > 1024 * 1024 * 1024) // Max 1GB
        {
            return {};
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), size);
        
        return data;
    }
}

bool VirtualFileSystem::IsImageFile(const std::wstring& filename)
{
    if (filename.empty())
        return false;

    auto ext = ToLower(filename);
    auto dotPos = ext.find_last_of(L'.');
    if (dotPos == std::wstring::npos)
        return false;

    ext = ext.substr(dotPos);
    const auto imageExts = GetSupportedImageExtensions();
    return std::find(imageExts.begin(), imageExts.end(), ext) != imageExts.end();
}

bool VirtualFileSystem::IsImageFile(const VirtualPath& path)
{
    return IsImageFile(path.GetFilename());
}

std::vector<std::wstring> VirtualFileSystem::GetImageExtensions()
{
    return GetSupportedImageExtensions();
}

std::vector<VirtualFileEntry> VirtualFileSystem::FilterImageEntries(const std::vector<VirtualFileEntry>& entries)
{
    std::vector<VirtualFileEntry> result;
    result.reserve(entries.size());

    for (const auto& entry : entries)
    {
        // Include directories, archives, and image files
        if (entry.isDirectory || entry.path.IsArchiveFile() || IsImageFile(entry.path))
        {
            result.push_back(entry);
        }
    }

    return result;
}

bool VirtualFileSystem::IsDirectory(const VirtualPath& path)
{
    if (path.IsInArchive())
    {
        // Check if it's a directory within archive
        auto reader = ArchiveReaderFactory::Open(path.hostPath);
        if (!reader)
        {
            return false;
        }

        std::wstring normalized = NormalizePath(path.archiveInnerPath);
        
        // Check if any entry starts with this path followed by '/'
        // This indicates it's a directory
        std::wstring searchPrefix = normalized;
        if (!searchPrefix.empty() && searchPrefix.back() != L'/')
        {
            searchPrefix += L'/';
        }

        auto entries = reader->ListEntries();
        for (const auto& entry : entries)
        {
            std::wstring entryNormalized = NormalizePath(entry.name);
            
            // If the entry is exactly this path and is marked as directory
            if (entryNormalized == normalized && entry.isDirectory)
            {
                return true;
            }
            
            // If any entry starts with this path + '/', it's a directory
            if (StartsWith(entryNormalized, searchPrefix))
            {
                return true;
            }
        }
        
        return false;
    }
    else if (path.IsArchiveFile())
    {
        // Archives act as directories
        return true;
    }
    else
    {
        return std::filesystem::is_directory(path.hostPath);
    }
}

std::vector<VirtualPath> VirtualFileSystem::GetAllImages(const VirtualPath& rootPath, bool recursive)
{
    std::vector<VirtualPath> images;
    std::vector<VirtualPath> dirsToProcess = { rootPath };

    while (!dirsToProcess.empty())
    {
        VirtualPath currentDir = dirsToProcess.back();
        dirsToProcess.pop_back();

        auto entries = ListDirectory(currentDir);

        for (const auto& entry : entries)
        {
            if (entry.isDirectory || entry.path.IsArchiveFile())
            {
                if (recursive)
                {
                    dirsToProcess.push_back(entry.path);
                }
            }
            else if (IsImageFile(entry.path))
            {
                images.push_back(entry.path);
            }
        }
    }

    return images;
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListFilesystemDirectory(const std::filesystem::path& dirPath)
{
    std::vector<VirtualFileEntry> entries;

    if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath))
    {
        return entries;
    }

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath))
        {
            VirtualPath vpath(entry.path());
            bool isDir = entry.is_directory();
            uint64_t size = isDir ? 0 : entry.file_size();
            
            auto ftime = entry.last_write_time();
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            time_t modTime = std::chrono::system_clock::to_time_t(sctp);

            entries.emplace_back(vpath, isDir, size, modTime);
        }
    }
    catch (...)
    {
        // Ignore errors
    }

    return entries;
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListArchiveRoot(const std::filesystem::path& archivePath)
{
    std::vector<VirtualFileEntry> entries;

    auto reader = ArchiveReaderFactory::Open(archivePath);
    if (!reader)
    {
        return entries;
    }

    auto archiveEntries = reader->ListEntries();
    std::unordered_set<std::wstring> addedDirs;

    for (const auto& archEntry : archiveEntries)
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        
        // Find first path separator
        auto sepPos = normalized.find(L'/');
        
        if (sepPos == std::wstring::npos)
        {
            // Direct child at archive root: can be either file or explicit directory entry.
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            // File in subdirectory - add the subdirectory if not already added
            std::wstring dirName = normalized.substr(0, sepPos);
            if (addedDirs.insert(dirName).second)
            {
                VirtualPath vpath(archivePath, dirName);
                entries.emplace_back(vpath, true, 0, 0);
            }
        }
    }

    return entries;
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListArchiveSubdirectory(
    const std::filesystem::path& archivePath,
    const std::wstring& innerPath)
{
    std::vector<VirtualFileEntry> entries;

    auto reader = ArchiveReaderFactory::Open(archivePath);
    if (!reader)
    {
        return entries;
    }

    std::wstring searchPrefix = NormalizePath(innerPath);
    if (!searchPrefix.empty() && searchPrefix.back() != L'/')
    {
        searchPrefix += L'/';
    }

    auto archiveEntries = reader->ListEntries();
    std::unordered_set<std::wstring> addedDirs;

    for (const auto& archEntry : archiveEntries)
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        
        // Check if this entry is under our search path
        if (!StartsWith(normalized, searchPrefix))
        {
            continue;
        }

        // Get relative path from search prefix
        std::wstring relativePath = normalized.substr(searchPrefix.length());
        
        // Find next path separator
        auto sepPos = relativePath.find(L'/');
        
        if (sepPos == std::wstring::npos)
        {
            // Direct child entry: can be file or explicit directory entry.
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            // File in subdirectory - add the subdirectory if not already added
            std::wstring dirName = relativePath.substr(0, sepPos);
            std::wstring fullDirPath = searchPrefix + dirName;
            
            if (addedDirs.insert(fullDirPath).second)
            {
                VirtualPath vpath(archivePath, fullDirPath);
                entries.emplace_back(vpath, true, 0, 0);
            }
        }
    }

    return entries;
}

std::wstring VirtualFileSystem::NormalizePath(const std::wstring& path)
{
    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'\\', L'/');
    
    // Remove trailing slash
    while (!result.empty() && result.back() == L'/')
    {
        result.pop_back();
    }
    
    return result;
}
