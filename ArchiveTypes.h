#pragma once

#include "CommonUtil.h"

#include <string>
#include <vector>

// Default ON when the build system does not define it (mirrors ArchiveReader.cpp).
#ifndef FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES
#define FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES 1
#endif

// Central definition of archive extension sets and display helpers.
// Keep these in one place so the reader, thumbnail strip, and info panel agree.
namespace ArchiveTypes
{
    // Bethesda archives (rendered with a blue icon tint in the thumbnail strip).
    inline bool IsBethesdaArchiveExt(const std::wstring& extLower)
    {
        return extLower == L".bsa" || extLower == L".ba2";
    }

    // Common archives handled via libarchive (rendered with a red icon tint).
    inline bool IsCommonArchiveExt(const std::wstring& extLower)
    {
        return extLower == L".zip" || extLower == L".7z" || extLower == L".rar";
    }

    inline bool IsKnownArchiveExt(const std::wstring& extLower)
    {
        return IsBethesdaArchiveExt(extLower) || IsCommonArchiveExt(extLower);
    }

    // Extensions the ArchiveReaderFactory can actually open (build-config dependent).
    inline const std::vector<std::wstring>& SupportedReadExtensions()
    {
        static const std::vector<std::wstring> exts = {
#if FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES
            L".zip", L".7z", L".rar",
#endif
            L".ba2"
        };
        return exts;
    }

    // Uppercase label without the leading dot (e.g. L".zip" -> L"ZIP").
    // Returns an empty string for unknown extensions.
    inline std::wstring BadgeLabelForExt(const std::wstring& extLower)
    {
        if (!IsKnownArchiveExt(extLower))
        {
            return L"";
        }

        std::wstring label = extLower;
        if (!label.empty() && label.front() == L'.')
        {
            label.erase(label.begin());
        }
        return CommonUtil::ToUpper(label);
    }
}
