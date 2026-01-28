#include "AppSetup.h"

#include "framework.h"

#include <filesystem>
#include <vector>

#include <shlobj.h>
#include <shobjidl.h>
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

    void LaunchAssociationUiForApp(const std::wstring& appName)
    {
        if (appName.empty())
        {
            return;
        }

        bool comInitedHere = false;

        const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hrInit))
        {
            comInitedHere = true;
        }
        else if (hrInit != RPC_E_CHANGED_MODE)
        {
            return;
        }
        IApplicationAssociationRegistrationUI* ui = nullptr;
        HRESULT hr = CoCreateInstance(
            CLSID_ApplicationAssociationRegistrationUI,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&ui));
        if (SUCCEEDED(hr) && ui)
        {
            #ifdef _DEBUG
            wchar_t dbgBuf[256];
            swprintf_s(dbgBuf, L"[FICture2] Calling LaunchAdvancedAssociationUI with: '%s'\n", appName.c_str());
            OutputDebugStringW(dbgBuf);
            #endif
            
            ShellExecuteW(nullptr, L"open",
                L"ms-settings:apps-defaults?app=FICture2",

                nullptr, nullptr, SW_SHOWNORMAL);

            // auto _appName = std::wstring(L"ImageGlass");
            // hr = ui->LaunchAdvancedAssociationUI(_appName.c_str());
            
            #ifdef _DEBUG
            if (FAILED(hr))
            {
                wchar_t buf[256];
                swprintf_s(buf, L"[FICture2] LaunchAdvancedAssociationUI failed: HRESULT = 0x%08X (%d)\n", hr, HRESULT_CODE(hr));
                OutputDebugStringW(buf);
            }
            else
            {
                OutputDebugStringW(L"[FICture2] LaunchAdvancedAssociationUI succeeded\n");
            }
            #endif
            
            ui->Release();
        }
        #ifdef _DEBUG
        else
        {
            wchar_t buf[256];
            swprintf_s(buf, L"[FICture2] CoCreateInstance failed: HRESULT = 0x%08X\n", hr);
            OutputDebugStringW(buf);
        }
        #endif

        if (comInitedHere)
            CoUninitialize();
    }

    void CleanupLegacyRegistrations(const std::wstring& exeName)
    {
#if FICTURE2_BUILD_FLAVOR_STANDALONE
        // For Standalone builds, clean up modern Capabilities registration (if exists from previous versions)
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FICture2\\Capabilities");
        
        HKEY regAppsKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &regAppsKey) == ERROR_SUCCESS)
        {
            RegDeleteValueW(regAppsKey, L"FICture2");
            RegCloseKey(regAppsKey);
        }
#else
        // For Store/Winget builds, clean up old Applications registration that causes duplicate entries
        if (!exeName.empty())
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\Applications\\" + exeName).c_str());
        }

        // Remove direct extension mappings (no longer used with Capabilities)
        const std::vector<std::wstring> exts = {
            L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff",
            L".gif", L".dds", L".tga", L".ico"
        };
        
        for (const auto& ext : exts)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + ext).c_str(), 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
            {
                wchar_t value[256] = {};
                DWORD size = sizeof(value);
                if (RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS)
                {
                    if (std::wstring(value) == L"FICture2.Image")
                    {
                        // Only delete our own registration
                        RegDeleteValueW(key, nullptr);
                    }
                }
                RegCloseKey(key);
            }
        }
#endif
    }

    bool RegisterPerUserFileAssociations(const std::wstring& exePath, const std::vector<std::wstring>& extensions)
    {
        if (exePath.empty() || extensions.empty())
        {
            return false;
        }

        const std::wstring appName = L"FICture2";
        std::wstring exeName = std::filesystem::path(exePath).filename().wstring();

        // Clean up legacy registrations that cause duplicate entries in Default Apps
        CleanupLegacyRegistrations(exeName);

        bool ok = true;
        const std::wstring progId = L"FICture2.Image";
        const std::wstring cmd = L"\"" + exePath + L"\" \"%1\"";
        const std::wstring icon = L"\"" + exePath + L"\",0";

        // Always register ProgID (HKCU only)
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, nullptr, L"FICture2 Image");
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\DefaultIcon", nullptr, icon);
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\shell\\open\\command", nullptr, cmd);

#if FICTURE2_BUILD_FLAVOR_STANDALONE
        // Standalone/Nexus: Use legacy Applications registration + direct extension mapping
        // This works without admin rights and doesn't require HKLM
        if (!exeName.empty())
        {
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\shell\\open\\command", nullptr, cmd);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\DefaultIcon", nullptr, icon);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName, L"FriendlyAppName", L"FICture2");

            for (const auto& ext : extensions)
            {
                if (ext.empty() || ext[0] != L'.')
                {
                    continue;
                }
                // SupportedTypes
                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\SupportedTypes\\" + ext, nullptr, L"");
            }
        }

        // Direct extension mapping for immediate file opening
        for (const auto& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
            {
                continue;
            }
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, nullptr, progId);
        }
#else
        // Store/Winget: Use modern Capabilities registration
        // Installer should handle HKLM registration for these builds
        const std::wstring capabilitiesKey = L"Software\\" + appName + L"\\Capabilities";
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, capabilitiesKey, L"ApplicationName", L"FICture2");
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, capabilitiesKey, L"ApplicationDescription", L"FICture2 Image Viewer");
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", appName.c_str(), L"Software\\" + appName + L"\\Capabilities");

        for (const auto& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
            {
                continue;
            }

            ok = ok && SetRegSzValue(
                HKEY_CURRENT_USER,
                capabilitiesKey + L"\\FileAssociations",
                ext.c_str(),
                progId);
        }
#endif

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
    namespace
    {
        std::vector<std::wstring> GetSupportedImageExtensions()
        {
            return {
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
        }
    }

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

#if !FICTURE2_ENABLE_FIRST_RUN_PROMPTS
        EnsureIniFileExists(iniFile, false);
        return;
#endif
        bool enabled = RegisterSupportedFileAssociations(NULL);
        /*
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

            const std::vector<std::wstring> exts = GetSupportedImageExtensions();

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
        */

        const int thumbChoice = MessageBoxW(
            nullptr,
            L"Register the DDS thumbnail provider for Windows Explorer?\n"
            L"(Requires Administrator approval.)\n\n"
            L"Do you want to apply this now?",
            L"FICture2 - Thumbnail Provider",
            MB_ICONQUESTION | MB_YESNO);
        if (thumbChoice == IDYES)
        {
            RegisterThumbnailProvider(nullptr, false);
        }

        // Create the INI so we don't ask again on the next run.
        EnsureIniFileExists(iniFile, enabled);
    }

    bool RegisterSupportedFileAssociations(HWND owner)
    {
#if !FICTURE2_BUILD_FLAVOR_STANDALONE
        // For winget/Store builds, check if already registered by installer (HKLM)
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RegisteredApplications", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            wchar_t value[512] = {};
            DWORD size = sizeof(value);
            if (RegQueryValueExW(key, L"FICture2", nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS)
            {
                RegCloseKey(key);
                
                // Already registered system-wide by installer
                MessageBoxW(
                    owner,
                    L"FICture2 is already registered with Windows.\n\n"
                    L"To set FICture2 as your default image viewer:\n"
                    L"1. Open Windows Settings\n"
                    L"2. Go to Apps > Default apps\n"
                    L"3. Search for 'FICture2'\n"
                    L"4. Set it as default for image file types",
                    L"FICture2 - File Associations",
                    MB_OK | MB_ICONINFORMATION);
                return true;
            }
            RegCloseKey(key);
        }
#endif

#if FICTURE2_BUILD_FLAVOR_STANDALONE
        const wchar_t* msg =
            L"Register FICture2 as the default image viewer for supported types?\n"
            L"(This will configure per-user associations.)\n\n"
            L"Extensions:\n"
            L".png .jpg .jpeg .bmp .tif .tiff .gif .dds .tga\n\n"
            L"Do you want to apply this now?";
#else
        const wchar_t* msg =
            L"Register FICture2 capabilities with Windows.\n\n"
            L"After registration, you can set FICture2 as default for image files in:\n"
            L"Windows Settings > Apps > Default apps\n\n"
            L"Extensions:\n"
            L".png .jpg .jpeg .bmp .tif .tiff .gif .dds .tga\n\n"
            L"Do you want to register now?";
#endif

        const int choice = MessageBoxW(
            owner,
            msg,
            L"FICture2 - File Associations",
            MB_ICONQUESTION | MB_YESNO);
        if (choice != IDYES)
        {
            return false;
        }

        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

        const std::vector<std::wstring> exts = GetSupportedImageExtensions();
        const bool enabled = RegisterPerUserFileAssociations(exePath, exts);

        const std::wstring iniFile = GetIniFilePath();
        if (!iniFile.empty())
        {
            WriteIniInt(iniFile, L"General", L"AskedAssociations", 1);
            WriteIniInt(iniFile, L"General", L"AssociationsEnabled", enabled ? 1 : 0);
        }

        if (!enabled)
        {
            MessageBoxW(
                owner,
                L"Failed to configure file associations.\n\n"
                L"Please check Windows Settings > Apps > Default apps to set FICture2 manually.",
                L"FICture2",
                MB_OK | MB_ICONWARNING);
            return false;
        }

#if FICTURE2_BUILD_FLAVOR_STANDALONE
        // Standalone: Direct registration works immediately
        MessageBoxW(
            owner,
            L"File associations updated successfully.\n\n"
            L"FICture2 is now registered for supported image formats.",
            L"FICture2",
            MB_OK | MB_ICONINFORMATION);
#else
        // Store/Winget: User needs to set manually in Windows Settings
        MessageBoxW(
            owner,
            L"File associations registered successfully.\n\n"
            L"To set FICture2 as your default image viewer:\n"
            L"1. Open Windows Settings\n"
            L"2. Go to Apps > Default apps\n"
            L"3. Search for 'FICture2'\n"
            L"4. Set it as default for image file types",
            L"FICture2",
            MB_OK | MB_ICONINFORMATION);
#endif

        return enabled;
    }

    void RegisterThumbnailProvider(HWND owner, bool unregister)
    {
        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

        std::filesystem::path dllPath = std::filesystem::path(exePath).parent_path() / L"ThumbnailProvider.dll";
        if (!std::filesystem::exists(dllPath))
        {
            std::wstring msg = L"ThumbnailProvider.dll not found.\n\nExpected location:\n";
            msg += dllPath.wstring();
            MessageBoxW(owner, msg.c_str(), L"FICture2", MB_OK | MB_ICONWARNING);
            return;
        }

        std::wstring args;
        if (unregister)
        {
            args = L"/u \"" + dllPath.wstring() + L"\"";
        }
        else
        {
            args = L"\"" + dllPath.wstring() + L"\"";
        }

        const HINSTANCE result = ShellExecuteW(
            owner,
            L"runas",
            L"regsvr32.exe",
            args.c_str(),
            nullptr,
            SW_SHOWNORMAL);

        if (reinterpret_cast<intptr_t>(result) <= 32)
        {
            MessageBoxW(
                owner,
                L"Failed to launch regsvr32 with elevation.\n\nPlease run as Administrator.",
                L"FICture2",
                MB_OK | MB_ICONWARNING);
        }
    }

    bool IsThumbnailProviderRegistered()
    {
        const wchar_t* kShellExtKey = L"Software\\Classes\\.dds\\ShellEx\\{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}";
        const wchar_t* kProviderClsid = L"{8b0a3d42-7022-4e35-b45f-7321b3e93c16}";

        auto hasProvider = [kShellExtKey, kProviderClsid](HKEY root) -> bool
        {
            wchar_t value[128] {};
            DWORD valueBytes = sizeof(value);
            const LONG rc = RegGetValueW(
                root,
                kShellExtKey,
                nullptr,
                RRF_RT_REG_SZ,
                nullptr,
                value,
                &valueBytes);
            if (rc != ERROR_SUCCESS)
            {
                return false;
            }

            return _wcsicmp(value, kProviderClsid) == 0;
        };

        return hasProvider(HKEY_CURRENT_USER) || hasProvider(HKEY_LOCAL_MACHINE);
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

