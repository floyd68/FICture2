#include "VirtualFileSystem.h"
#include "ArchiveReader.h"
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
            return {};
        }
        return reader->ExtractToMemory(filePath.archiveInnerPath);
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

    static const std::unordered_set<std::wstring> imageExts = {
        L".dds", L".png", L".jpg", L".jpeg", L".bmp", L".tga", L".tif", L".tiff",
        L".gif", L".webp", L".hdr", L".exr", L".psd"
    };

    return imageExts.find(ext) != imageExts.end();
}

bool VirtualFileSystem::IsImageFile(const VirtualPath& path)
{
    return IsImageFile(path.GetFilename());
}

std::vector<std::wstring> VirtualFileSystem::GetImageExtensions()
{
    return {
        L".dds", L".png", L".jpg", L".jpeg", L".bmp", L".tga", L".tif", L".tiff",
        L".gif", L".webp", L".hdr", L".exr", L".psd"
    };
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
        // This would require scanning the archive - for now, assume it's a file
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
        OutputDebugStringW((L"[VFS] Failed to open archive: " + archivePath.wstring() + L"\n").c_str());
        return entries;
    }

    auto archiveEntries = reader->ListEntries();
    OutputDebugStringW((L"[VFS] Archive has " + std::to_wstring(archiveEntries.size()) + L" entries\n").c_str());
    std::unordered_set<std::wstring> addedDirs;

    for (const auto& archEntry : archiveEntries)
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        
        // Find first path separator
        auto sepPos = normalized.find(L'/');
        
        if (sepPos == std::wstring::npos)
        {
            // Root-level file
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, false, archEntry.size, archEntry.modTime);
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
            // Direct child file
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, false, archEntry.size, archEntry.modTime);
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
