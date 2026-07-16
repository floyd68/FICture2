#include "ImageBrowserSessionPersistence.h"
#include "IniStore.h"
#include "CommonUtil.h"
#include "ImageBrowserSplitCoordinator.h"

#include <Windows.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <string_view>

namespace ImageBrowserSessionPersistence
{
    namespace
    {
        std::wstring JoinFloatsCsv(const std::vector<float>& values)
        {
            std::wstring s;
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                {
                    s += L',';
                }
                s += std::format(L"{:.6f}", values[i]);
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
                const std::wstring_view token(s.data() + start, end - start);
                if (const auto value = CommonUtil::TryParseFloat(token))
                {
                    out.push_back(*value);
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

        const int clampedCount = (std::max)(
            0,
            (std::min)(static_cast<int>(ImageBrowserSplitCoordinator::kMaxViewers), payload.viewerCount));

        IniStore::SetInt(iniFile, L"Session", L"ViewerCount", clampedCount);

        if (payload.hasThumbStripHeight)
        {
            IniStore::SetString(
                iniFile,
                L"Session",
                L"ThumbStripHeight",
                std::format(L"{:.2f}", payload.thumbStripHeight));
        }

        IniStore::SetString(
            iniFile,
            L"Session",
            L"HorizontalSplitRatios",
            JoinFloatsCsv(payload.horizontalSplitRatios));

        for (int i = 0; i < clampedCount; ++i)
        {
            if (static_cast<size_t>(i) >= payload.viewers.size())
            {
                break;
            }

            const auto& viewer = payload.viewers[static_cast<size_t>(i)];
            const std::wstring sec = L"Viewer" + std::to_wstring(i);
            IniStore::SetString(iniFile, sec, L"DisplayedFile", viewer.displayedFile);
            IniStore::SetString(iniFile, sec, L"CurrentFolder", viewer.currentFolder);
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
        const auto ini = IniStore::Load(iniFile);
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
        outPayload.clampedViewerCount = (std::max)(
            1,
            (std::min)(static_cast<int>(ImageBrowserSplitCoordinator::kMaxViewers), outPayload.viewerCount));

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
