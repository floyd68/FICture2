#pragma once

#include <string>
#include <vector>

namespace RecentFiles
{
    constexpr std::size_t kMaxEntries = 12;

    std::vector<std::wstring> Load(const std::wstring& iniPath);
    void Add(const std::wstring& iniPath, const std::wstring& path);
    void Clear(const std::wstring& iniPath);

    // Menu label: trim long paths and escape '&' mnemonics.
    std::wstring MenuLabel(const std::wstring& path);
}
