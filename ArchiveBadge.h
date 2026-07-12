#pragma once

#include "ArchiveTypes.h"
#include "CommonUtil.h"

#include <string>

// Display badge for archive extensions (UI-only; kept out of Floar::ArchiveTypes).
namespace ArchiveBadge
{
    // Uppercase label without the leading dot (e.g. L".zip" -> L"ZIP").
    // Returns an empty string for unknown extensions.
    inline std::wstring BadgeLabelForExt(const std::wstring& extLower)
    {
        if (!Floar::ArchiveTypes::IsKnownArchiveExt(extLower))
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
