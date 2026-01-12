#include "AppSetup.h"

#include "framework.h"

#include <filesystem>
#include <vector>

#include <shlobj.h>
#include <shellapi.h>
#include <winreg.h>

namespace
{
    void WriteIniDefaults(const std::wstring& iniFile)
    {
        if (iniFile.empty())
        {
            return;
        }

        // General
        (void)WritePrivateProfileStringW(L"General", L"IniVersion", L"1", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"General", L"Initialized", L"1", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"General", L"AskedAssociations", L"0", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"General", L"AssociationsEnabled", L"0", iniFile.c_str());

        // Image / viewer behavior
        (void)WritePrivateProfileStringW(L"Image", L"ZoomStiffness", L"80.0", iniFile.c_str());

        // Viewer defaults (reserved for future expansion)
        (void)WritePrivateProfileStringW(L"Viewer", L"PaneCount", L"1", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"Viewer", L"ShowNavItems", L"1", iniFile.c_str());

        // Thumbnail strip defaults (reserved for future expansion)
        (void)WritePrivateProfileStringW(L"Thumbnails", L"MinSize", L"32", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"Thumbnails", L"MaxSize", L"256", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"Thumbnails", L"ItemSpacing", L"8", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"Thumbnails", L"Padding", L"8", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"Thumbnails", L"TileLabelSpacing", L"2", iniFile.c_str());

        // Window placement (will be populated on first exit).
        (void)WritePrivateProfileStringW(L"Window", L"ShowCmd", L"1", iniFile.c_str()); // SW_SHOWNORMAL

        // Background color (R,G,B 0-255). Default matches previous clear: (0.09,0.09,0.10) ~ (23,23,26)
        (void)WritePrivateProfileStringW(L"Window", L"BackgroundColor", L"20,23,23", iniFile.c_str());

        // Focused ImageBrowser background color (R,G,B 0-255). Default: dark yellow accent.
        // (0.18,0.16,0.03) ~ (46,41,8)
        (void)WritePrivateProfileStringW(L"Window", L"FocusedBackgroundColor", L"35,43,43", iniFile.c_str());
    }

    void EnsureIniFileExists(const std::wstring& iniFile, bool associationsEnabled)
    {
        if (iniFile.empty())
        {
            return;
        }

        if (std::filesystem::exists(iniFile))
        {
            return;
        }

        try
        {
            std::filesystem::path p = iniFile;
            std::filesystem::create_directories(p.parent_path());
        }
        catch (...)
        {
            // Best-effort.
        }

        // Create an INI with full default settings so the user can see/edit everything.
        WriteIniDefaults(iniFile);

        // First-run prompt result.
        (void)WritePrivateProfileStringW(L"General", L"AskedAssociations", L"1", iniFile.c_str());
        (void)WritePrivateProfileStringW(L"General", L"AssociationsEnabled", associationsEnabled ? L"1" : L"0", iniFile.c_str());
    }

    bool SetRegSzValue(HKEY root, const std::wstring& subKey, const wchar_t* valueNameOrNull, const std::wstring& value)
    {
        HKEY key = nullptr;
        DWORD disp = 0;
        const LONG rc = RegCreateKeyExW(
            root,
            subKey.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &key,
            &disp);
        if (rc != ERROR_SUCCESS)
        {
            return false;
        }

        const wchar_t* valueName = valueNameOrNull; // nullptr => (Default)
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        const LONG rc2 = RegSetValueExW(
            key,
            valueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            bytes);
        RegCloseKey(key);
        return rc2 == ERROR_SUCCESS;
    }

    bool RegisterPerUserFileAssociations(const std::wstring& exePath, const std::vector<std::wstring>& extensions)
    {
        if (exePath.empty() || extensions.empty())
        {
            return false;
        }

        const std::wstring progId = L"FICture2.Image";
        const std::wstring cmd = L"\"" + exePath + L"\" \"%1\"";
        const std::wstring icon = L"\"" + exePath + L"\",0";

        // ProgID registration (HKCU only).
        bool ok = true;
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, nullptr, L"FICture2 Image");
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\DefaultIcon", nullptr, icon);
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\shell\\open\\command", nullptr, cmd);

        // Application registration (helps Windows discover supported types; still HKCU).
        std::wstring exeName = std::filesystem::path(exePath).filename().wstring();
        if (!exeName.empty())
        {
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\shell\\open\\command", nullptr, cmd);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\DefaultIcon", nullptr, icon);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName, L"FriendlyAppName", L"FICture2");

            for (const auto& ext : extensions)
            {
                // Presence indicates support.
                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\SupportedTypes\\" + ext, nullptr, L"");
            }
        }

        // Extension -> ProgID mapping (HKCU only).
        for (const auto& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
            {
                continue;
            }

            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, nullptr, progId);
            (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, L"PerceivedType", L"image");
        }

        // Notify Explorer that associations changed.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return ok;
    }

    bool ReadIniInt(const std::wstring& iniFile, const wchar_t* section, const wchar_t* key, int& outValue)
    {
        if (iniFile.empty() || section == nullptr || key == nullptr)
        {
            return false;
        }

        wchar_t buf[64] {};
        const DWORD n = GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
        if (n == 0)
        {
            return false;
        }

        outValue = _wtoi(buf);
        return true;
    }

    void WriteIniInt(const std::wstring& iniFile, const wchar_t* section, const wchar_t* key, int value)
    {
        if (iniFile.empty() || section == nullptr || key == nullptr)
        {
            return;
        }

        wchar_t buf[64] {};
        _itow_s(value, buf, 10);
        (void)WritePrivateProfileStringW(section, key, buf, iniFile.c_str());
    }

    void ClampRectToMonitorWorkArea(RECT& rc)
    {
        // If the rect is invalid, bail.
        if (rc.right <= rc.left || rc.bottom <= rc.top)
        {
            return;
        }

        const HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
        {
            return;
        }

        const RECT wa = mi.rcWork;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;

        // Ensure at least some portion is visible.
        const int minVisible = 64;

        // Clamp size (avoid absurd sizes if INI got corrupted).
        const int maxW = (wa.right - wa.left);
        const int maxH = (wa.bottom - wa.top);
        const int clampedW = (std::max)(200, (std::min)(w, maxW));
        const int clampedH = (std::max)(200, (std::min)(h, maxH));

        rc.right = rc.left + clampedW;
        rc.bottom = rc.top + clampedH;

        if (rc.left > wa.right - minVisible)
        {
            rc.left = wa.right - minVisible;
            rc.right = rc.left + clampedW;
        }
        if (rc.top > wa.bottom - minVisible)
        {
            rc.top = wa.bottom - minVisible;
            rc.bottom = rc.top + clampedH;
        }
        if (rc.right < wa.left + minVisible)
        {
            rc.right = wa.left + minVisible;
            rc.left = rc.right - clampedW;
        }
        if (rc.bottom < wa.top + minVisible)
        {
            rc.bottom = wa.top + minVisible;
            rc.top = rc.bottom - clampedH;
        }

        // Final clamp to work area bounds (allow partially offscreen, but not fully).
        if (rc.left < wa.left)
        {
            rc.left = wa.left;
            rc.right = rc.left + clampedW;
        }
        if (rc.top < wa.top)
        {
            rc.top = wa.top;
            rc.bottom = rc.top + clampedH;
        }
        if (rc.right > wa.right)
        {
            rc.right = wa.right;
            rc.left = rc.right - clampedW;
        }
        if (rc.bottom > wa.bottom)
        {
            rc.bottom = wa.bottom;
            rc.top = rc.bottom - clampedH;
        }
    }
}

namespace FICture2App
{
    std::wstring GetIniFilePath()
    {
        wchar_t iniPath[MAX_PATH] {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
        {
            return {};
        }

        return std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
    }

    void RunFirstRunAssociationPromptIfNeeded()
    {
        const std::wstring iniFile = GetIniFilePath();
        const bool firstRun = !iniFile.empty() && !std::filesystem::exists(iniFile);
        if (!firstRun)
        {
            return;
        }

        const wchar_t* msg =
            L"Set FICture2 as your default image viewer?\n"
            L"(This will configure per-user (HKCU) associations only, not system-wide.)\n\n"
            L"Extensions:\n"
            L".png .jpg .jpeg .bmp .tif .tiff .gif .dds .tga\n\n"
            L"Do you want to apply this now?";

        const int choice = MessageBoxW(nullptr, msg, L"FICture2 - File Associations", MB_ICONQUESTION | MB_YESNO);

        bool enabled = false;
        if (choice == IDYES)
        {
            wchar_t exePath[MAX_PATH] {};
            GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

            const std::vector<std::wstring> exts =
            {
                L".png",
                L".jpg",
                L".jpeg",
                L".bmp",
                L".tif",
                L".tiff",
                L".gif",
                L".dds",
                L".tga",
            };

            enabled = RegisterPerUserFileAssociations(exePath, exts);

            if (!enabled)
            {
                MessageBoxW(
                    nullptr,
                    L"Failed to configure file associations.\n\n"
                    L"Depending on your Windows version/policy, apps may not be able to set default apps automatically.\n"
                    L"If needed, set FICture2 manually in Windows Settings > Default apps.",
                    L"FICture2",
                    MB_OK | MB_ICONWARNING);
            }
        }

        // Create the INI so we don't ask again on the next run.
        EnsureIniFileExists(iniFile, enabled);
    }

    void LoadWindowPlacement(HWND hwnd)
    {
        if (hwnd == nullptr)
        {
            return;
        }

        const std::wstring iniFile = GetIniFilePath();
        if (iniFile.empty() || !std::filesystem::exists(iniFile))
        {
            return;
        }

        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        int showCmd = 0;

        if (!ReadIniInt(iniFile, L"Window", L"Left", left) ||
            !ReadIniInt(iniFile, L"Window", L"Top", top) ||
            !ReadIniInt(iniFile, L"Window", L"Right", right) ||
            !ReadIniInt(iniFile, L"Window", L"Bottom", bottom) ||
            !ReadIniInt(iniFile, L"Window", L"ShowCmd", showCmd))
        {
            return;
        }

        RECT rc { left, top, right, bottom };
        ClampRectToMonitorWorkArea(rc);

        WINDOWPLACEMENT wp {};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hwnd, &wp))
        {
            return;
        }

        wp.rcNormalPosition = rc;
        wp.showCmd = (showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        (void)SetWindowPlacement(hwnd, &wp);
    }

    void SaveWindowPlacement(HWND hwnd)
    {
        if (hwnd == nullptr)
        {
            return;
        }

        const std::wstring iniFile = GetIniFilePath();
        if (iniFile.empty())
        {
            return;
        }

        WINDOWPLACEMENT wp {};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hwnd, &wp))
        {
            return;
        }

        const RECT rc = wp.rcNormalPosition;
        WriteIniInt(iniFile, L"Window", L"Left", rc.left);
        WriteIniInt(iniFile, L"Window", L"Top", rc.top);
        WriteIniInt(iniFile, L"Window", L"Right", rc.right);
        WriteIniInt(iniFile, L"Window", L"Bottom", rc.bottom);
        WriteIniInt(iniFile, L"Window", L"ShowCmd", static_cast<int>(wp.showCmd));
    }
}

