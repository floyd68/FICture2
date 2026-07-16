#include "RecentFiles.h"
#include "SimpleIniFile.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    std::vector<std::wstring> SplitPipeList(std::wstring_view s)
    {
        std::vector<std::wstring> out;
        std::size_t start = 0;
        while (start < s.size())
        {
            std::size_t bar = s.find(L'|', start);
            if (bar == std::wstring_view::npos)
            {
                bar = s.size();
            }
            std::wstring_view part = s.substr(start, bar - start);
            while (!part.empty() && iswspace(part.front()))
            {
                part.remove_prefix(1);
            }
            while (!part.empty() && iswspace(part.back()))
            {
                part.remove_suffix(1);
            }
            if (!part.empty())
            {
                out.emplace_back(part);
            }
            start = bar + 1;
        }
        return out;
    }

    std::wstring JoinPipeList(const std::vector<std::wstring>& parts)
    {
        std::wstring out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out += L'|';
            }
            out += parts[i];
        }
        return out;
    }
}

namespace RecentFiles
{
    std::vector<std::wstring> Load(const std::wstring& iniPath)
    {
        if (iniPath.empty())
        {
            return {};
        }

        const SimpleIniFile ini = SimpleIniFile::Load(iniPath);
        std::vector<std::wstring> out = SplitPipeList(ini.GetString(L"Session", L"RecentFiles"));
        if (out.size() > kMaxEntries)
        {
            out.resize(kMaxEntries);
        }
        return out;
    }

    void Add(const std::wstring& iniPath, const std::wstring& path)
    {
        if (iniPath.empty() || path.empty())
        {
            return;
        }

        std::vector<std::wstring> files = Load(iniPath);
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

        (void)WritePrivateProfileStringW(
            L"Session",
            L"RecentFiles",
            JoinPipeList(files).c_str(),
            iniPath.c_str());
    }

    void Clear(const std::wstring& iniPath)
    {
        if (iniPath.empty())
        {
            return;
        }
        (void)WritePrivateProfileStringW(L"Session", L"RecentFiles", L"", iniPath.c_str());
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
