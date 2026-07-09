#include "ImageBrowserSessionPersistence.h"
#include "SimpleIniFile.h"

#include <Windows.h>

#include <algorithm>
#include <iterator>

namespace ImageBrowserSessionPersistence
{
    namespace
    {
        std::wstring JoinFloatsCsv(const std::vector<float>& values)
        {
            std::wstring s;
            for (size_t i = 0; i < values.size(); ++i)
            {
                wchar_t buf[64] {};
                swprintf_s(buf, L"%.6f", values[i]);
                if (i != 0)
                {
                    s += L",";
                }
                s += buf;
            }
            return s;
        }

        std::vector<float> ParseFloatsCsv(const std::wstring& s)
        {
            std::vector<float> out;
            size_t start = 0;
            while (start < s.size())
            {
                size_t end = s.find(L',', start);
                if (end == std::wstring::npos)
                {
                    end = s.size();
                }
                const std::wstring token = s.substr(start, end - start);
                if (!token.empty())
                {
                    out.push_back(static_cast<float>(_wtof(token.c_str())));
                }
                start = end + 1;
            }
            return out;
        }
    }

    void SaveToIni(const std::wstring& iniFile, const SavePayload& payload)
    {
        if (iniFile.empty())
        {
            return;
        }

        const int clampedCount = (std::max)(0, (std::min)(4, payload.viewerCount));

        wchar_t countBuf[32] {};
        _itow_s(clampedCount, countBuf, 10);
        (void)WritePrivateProfileStringW(L"Session", L"ViewerCount", countBuf, iniFile.c_str());

        if (payload.hasThumbStripHeight)
        {
            wchar_t thumbBuf[64] {};
            swprintf_s(thumbBuf, L"%.2f", payload.thumbStripHeight);
            (void)WritePrivateProfileStringW(L"Session", L"ThumbStripHeight", thumbBuf, iniFile.c_str());
        }

        const std::wstring ratiosCsv = JoinFloatsCsv(payload.horizontalSplitRatios);
        (void)WritePrivateProfileStringW(L"Session", L"HorizontalSplitRatios", ratiosCsv.c_str(), iniFile.c_str());

        for (int i = 0; i < clampedCount; ++i)
        {
            if (static_cast<size_t>(i) >= payload.viewers.size())
            {
                break;
            }

            const auto& viewer = payload.viewers[static_cast<size_t>(i)];
            const std::wstring sec = L"Viewer" + std::to_wstring(i);
            (void)WritePrivateProfileStringW(sec.c_str(), L"DisplayedFile", viewer.displayedFile.c_str(), iniFile.c_str());
            (void)WritePrivateProfileStringW(sec.c_str(), L"CurrentFolder", viewer.currentFolder.c_str(), iniFile.c_str());
        }
    }

    bool TryRestoreFromIni(const std::wstring& iniFile, RestorePayload& outPayload)
    {
        if (iniFile.empty())
        {
            return false;
        }

        // Read the file once; all individual GetPrivateProfile* calls
        // re-open the file on every call and cause significant startup latency.
        const auto ini = SimpleIniFile::Load(iniFile);
        if (!ini.IsLoaded())
        {
            return false;
        }

        outPayload = RestorePayload {};
        outPayload.viewerCount = ini.GetInt(L"Session", L"ViewerCount", 0);
        if (outPayload.viewerCount <= 0)
        {
            return false;
        }
        outPayload.clampedViewerCount = (std::max)(1, (std::min)(4, outPayload.viewerCount));

        const float thumbH = ini.GetFloat(L"Session", L"ThumbStripHeight", 0.0f);
        if (thumbH > 1.0f)
        {
            outPayload.hasThumbStripHeight = true;
            outPayload.thumbStripHeight = thumbH;
        }

        outPayload.viewers.reserve(static_cast<size_t>(outPayload.clampedViewerCount));
        for (int i = 0; i < outPayload.clampedViewerCount; ++i)
        {
            const std::wstring sec = L"Viewer" + std::to_wstring(i);
            std::wstring file   = ini.GetString(sec, L"DisplayedFile");
            std::wstring folder = ini.GetString(sec, L"CurrentFolder");
            outPayload.viewers.push_back({ std::move(file), std::move(folder) });
        }

        const std::wstring ratStr = ini.GetString(L"Session", L"HorizontalSplitRatios");
        if (!ratStr.empty())
        {
            outPayload.horizontalSplitRatios = ParseFloatsCsv(ratStr);
        }

        return true;
    }
}
