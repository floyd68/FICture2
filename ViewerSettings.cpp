#include "ViewerSettings.h"

#include "CommonUtil.h"
#include "IniStore.h"
#include "IniStoreLog.h"

#include <algorithm>
#include <cstdio>
#include <format>

std::optional<D2D1_COLOR_F> ViewerSettings::Load(const std::wstring& iniPath)
{
    if (m_loaded)
    {
        return std::nullopt;
    }
    m_loaded = true;
    if (iniPath.empty())
    {
        return std::nullopt;
    }

    // Read the entire INI file once; avoid calling GetPrivateProfile* APIs
    // which re-open and re-parse the file on every individual call.
    const auto ini = IniStore::Load(iniPath);
    if (!ini.IsLoaded())
    {
        return std::nullopt;
    }

    m_showNavItems = (ini.GetInt(L"Viewer", L"ShowNavItems", 1) != 0);

    std::optional<D2D1_COLOR_F> background {};
    D2D1_COLOR_F color {};
    if (ParseRgb(ini.GetString(L"Window", L"BackgroundColor"), color))
    {
        background = color;
    }
    if (ParseRgb(ini.GetString(L"Window", L"FocusedBackgroundColor"), color))
    {
        m_focusedBackgroundColor = color;
    }

    const float zoomStiffness = ini.GetFloat(L"Image", L"ZoomStiffness", kDefaultZoomStiffness);
    if (zoomStiffness >= kMinZoomStiffness && zoomStiffness <= kMaxZoomStiffness)
    {
        m_zoomStiffness = zoomStiffness;
    }
    return background;
}

void ViewerSettings::SetShowNavItems(bool show, const std::wstring& iniPath)
{
    m_showNavItems = show;
    if (!iniPath.empty())
    {
        LogIniWrite(IniStore::SetString(iniPath, L"Viewer", L"ShowNavItems", show ? L"1" : L"0"), "ShowNavItems");
    }
}

void ViewerSettings::SetFocusedBackgroundColor(const D2D1_COLOR_F& color, const std::wstring& iniPath)
{
    m_focusedBackgroundColor = color;
    if (!iniPath.empty())
    {
        LogIniWrite(IniStore::SetString(iniPath, L"Window", L"FocusedBackgroundColor", FormatRgb(color)), "Background color");
    }
}

void ViewerSettings::SaveBackgroundColor(const D2D1_COLOR_F& color, const std::wstring& iniPath)
{
    if (!iniPath.empty())
    {
        LogIniWrite(IniStore::SetString(iniPath, L"Window", L"BackgroundColor", FormatRgb(color)), "Background color");
    }
}

bool ViewerSettings::ParseRgb(const std::wstring& text, D2D1_COLOR_F& outColor)
{
    if (text.empty())
    {
        return false;
    }

    int r = -1;
    int g = -1;
    int b = -1;
    if (swscanf_s(text.c_str(), L"%d,%d,%d", &r, &g, &b) != 3)
    {
        return false;
    }

    r = (std::max)(0, (std::min)(255, r));
    g = (std::max)(0, (std::min)(255, g));
    b = (std::max)(0, (std::min)(255, b));
    outColor = D2D1::ColorF(
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        1.0f);
    return true;
}

std::wstring ViewerSettings::FormatRgb(const D2D1_COLOR_F& color)
{
    return std::format(
        L"{},{},{}",
        CommonUtil::ToByte255(color.r),
        CommonUtil::ToByte255(color.g),
        CommonUtil::ToByte255(color.b));
}
