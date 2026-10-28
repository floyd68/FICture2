#pragma once

#include "IniStore.h"

#include <string>
#include <vector>

namespace RecentFiles
{
    constexpr std::size_t kMaxEntries = 12;

    std::vector<std::wstring> Load(const std::wstring& iniPath);
    // Each is one IniStore transaction; a status other than Committed is logged.
    IniStore::WriteStatus Add(const std::wstring& iniPath, const std::wstring& path);
    IniStore::WriteStatus Clear(const std::wstring& iniPath);

    // Menu label: trim long paths and escape '&' mnemonics.
    std::wstring MenuLabel(const std::wstring& path);
}
