#pragma once

#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "CommonUtil.h"

#include <algorithm>
#include <vector>

// Image-aware helpers layered on Floar::VirtualFileSystem.
// Floar itself has no ImageCore dependency; these live in the FICture2 app.
namespace ImageAwareVfs
{
    // Direct registry lookup: no per-call copy of the supported-extension list.
    inline bool IsImageFile(const std::wstring& filename)
    {
        const auto dotPos = filename.find_last_of(L'.');
        if (filename.empty() || dotPos == std::wstring::npos)
        {
            return false;
        }

        ImageCore::RegisterBuiltInDecoders();
        return ImageCore::DecoderRegistry::Instance().IsSupportedExtension(
            std::wstring_view(filename).substr(dotPos));
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
