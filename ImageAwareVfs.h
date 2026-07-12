#pragma once

#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include "ImageCore/ImageDecodeDispatcher.h"
#include "CommonUtil.h"

#include <algorithm>
#include <vector>

// Image-aware helpers layered on Floar::VirtualFileSystem.
// Floar itself has no ImageCore dependency; these live in the FICture2 app.
namespace ImageAwareVfs
{
    inline bool IsImageFile(const std::wstring& filename)
    {
        if (filename.empty())
        {
            return false;
        }

        auto ext = CommonUtil::ToLower(filename);
        const auto dotPos = ext.find_last_of(L'.');
        if (dotPos == std::wstring::npos)
        {
            return false;
        }

        ext = ext.substr(dotPos);
        const auto imageExts = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        return std::find(imageExts.begin(), imageExts.end(), ext) != imageExts.end();
    }

    inline bool IsImageFile(const Floar::VirtualPath& path)
    {
        return IsImageFile(path.GetFilename());
    }

    inline std::vector<std::wstring> GetImageExtensions()
    {
        return ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
    }

    inline std::vector<Floar::VirtualFileEntry> FilterImageEntries(
        const std::vector<Floar::VirtualFileEntry>& entries)
    {
        std::vector<Floar::VirtualFileEntry> result;
        result.reserve(entries.size());

        for (const auto& entry : entries)
        {
            if (entry.isDirectory || entry.path.IsArchiveFile() || IsImageFile(entry.path))
            {
                result.push_back(entry);
            }
        }

        return result;
    }

    inline bool IsBrowsableDropTarget(const Floar::VirtualPath& path)
    {
        return Floar::VirtualFileSystem::IsDirectory(path) || path.IsArchiveFile() || IsImageFile(path);
    }

    inline std::vector<Floar::VirtualPath> GetAllImages(
        const Floar::VirtualPath& rootPath,
        bool recursive = false)
    {
        std::vector<Floar::VirtualPath> images;
        std::vector<Floar::VirtualPath> dirsToProcess = { rootPath };

        while (!dirsToProcess.empty())
        {
            Floar::VirtualPath currentDir = dirsToProcess.back();
            dirsToProcess.pop_back();

            auto entries = Floar::VirtualFileSystem::ListDirectory(currentDir);

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
}
