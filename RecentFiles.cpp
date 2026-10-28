#include "RecentFiles.h"
#include "IniStore.h"
#include "IniStoreLog.h"

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace RecentFiles
{
    std::vector<std::wstring> Load(const std::wstring& iniPath)
    {
        if (iniPath.empty())
        {
            return {};
        }

        const IniStore ini = IniStore::Load(iniPath);
        std::vector<std::wstring> out = IniStore::SplitPipeList(ini.GetString(L"Session", L"RecentFiles"));
        if (out.size() > kMaxEntries)
        {
            out.resize(kMaxEntries);
        }
        return out;
    }

    IniStore::WriteStatus Add(const std::wstring& iniPath, const std::wstring& path)
    {
        if (iniPath.empty() || path.empty())
        {
            return IniStore::WriteStatus::Committed;
        }

        // Read and write the list in one transaction, so another instance's
        // update between them is not lost.
        const IniStore::WriteStatus status = IniStore::Transact(iniPath, [&](IniStore& ini)
        {
            std::vector<std::wstring> files = IniStore::SplitPipeList(ini.GetString(L"Session", L"RecentFiles"));
            files.erase(
                std::remove_if(
                    files.begin(),
                    files.end(),
                    [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
                files.end());
            files.insert(files.begin(), path);
            if (files.size() > kMaxEntries)
            {
                files.resize(kMaxEntries);
            }
            ini.Put(L"Session", L"RecentFiles", IniStore::JoinPipeList(files));
        });
        return LogIniWrite(status, "RecentFiles");
    }

    IniStore::WriteStatus Clear(const std::wstring& iniPath)
    {
        if (iniPath.empty())
        {
            return IniStore::WriteStatus::Committed;
        }
        return LogIniWrite(IniStore::SetString(iniPath, L"Session", L"RecentFiles", L""), "RecentFiles");
    }

    std::wstring MenuLabel(const std::wstring& path)
    {
        constexpr std::size_t kMax = 64;
        std::wstring label = path.size() <= kMax
            ? path
            : L"..." + path.substr(path.size() - (kMax - 3));

        std::wstring escaped;
        escaped.reserve(label.size() + 4);
        for (const wchar_t c : label)
        {
            escaped += c;
            if (c == L'&')
            {
                escaped += L'&';
            }
        }
        return escaped;
    }
}
