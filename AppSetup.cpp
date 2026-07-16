#include "AppSetup.h"
#include "AppLog.h"
#include "CommonUtil.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "IniStore.h"

#include "framework.h"

#include <filesystem>
#include <format>
#include <vector>

#include <shlobj.h>
#include <shellapi.h>
#include <winreg.h>

namespace
{
    std::wstring BuildExtensionPromptLine(const std::vector<std::wstring>& exts)
    {
        std::wstring line {};
        for (size_t i = 0; i < exts.size(); ++i)
        {
            if (i != 0)
            {
                line += L' ';
            }
            line += exts[i];
        }
        return line;
    }

    void WriteIniDefaults(const std::wstring& iniFile)
    {
        if (iniFile.empty())
        {
            return;
        }

        // General
        IniStore::SetString(iniFile, L"General", L"IniVersion", L"1");
        IniStore::SetString(iniFile, L"General", L"Initialized", L"1");
        IniStore::SetString(iniFile, L"General", L"AskedAssociations", L"0");
        IniStore::SetString(iniFile, L"General", L"AssociationsEnabled", L"0");

        // Image / viewer behavior
        IniStore::SetString(iniFile, L"Image", L"ZoomStiffness", L"80.0");

        // Viewer defaults (reserved for future expansion)
        IniStore::SetString(iniFile, L"Viewer", L"PaneCount", L"1");
        IniStore::SetString(iniFile, L"Viewer", L"ShowNavItems", L"1");

        // Thumbnail strip defaults (reserved for future expansion)
        IniStore::SetString(iniFile, L"Thumbnails", L"MinSize", L"32");
        IniStore::SetString(iniFile, L"Thumbnails", L"MaxSize", L"256");
        IniStore::SetString(iniFile, L"Thumbnails", L"ItemSpacing", L"8");
        IniStore::SetString(iniFile, L"Thumbnails", L"Padding", L"8");
        IniStore::SetString(iniFile, L"Thumbnails", L"TileLabelSpacing", L"2");

        // Window placement (will be populated on first exit).
        IniStore::SetString(iniFile, L"Window", L"ShowCmd", L"1"); // SW_SHOWNORMAL

        // Background color (R,G,B 0-255). Default matches previous clear: (0.09,0.09,0.10) ~ (23,23,26)
        IniStore::SetString(iniFile, L"Window", L"BackgroundColor", L"20,23,23");

        // Focused ImageBrowser background color (R,G,B 0-255). Default: dark yellow accent.
        // (0.18,0.16,0.03) ~ (46,41,8)
        IniStore::SetString(iniFile, L"Window", L"FocusedBackgroundColor", L"35,43,43");
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
        IniStore::SetString(iniFile, L"General", L"AskedAssociations", L"1");
        IniStore::SetString(
            iniFile,
            L"General",
            L"AssociationsEnabled",
            associationsEnabled ? L"1" : L"0");
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
        const std::vector<std::wstring> exts = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        
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

        // Register modern Capabilities model for all flavors (HKCU).
        // This is required for Windows Default Apps UI integration.
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

        // Also register legacy Application + OpenWith metadata for broader shell compatibility.
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

                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\SupportedTypes\\" + ext, nullptr, L"");
                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext + L"\\OpenWithProgids\\" + progId, nullptr, L"");
            }
        }

#if FICTURE2_BUILD_FLAVOR_STANDALONE
        // Best-effort direct mapping (may be ignored when UserChoice policy is active).
        for (const auto& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
            {
                continue;
            }
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, nullptr, progId);
        }
#else
        // Store/Winget builds generally rely on installer/MSIX pathways for defaults.
#endif

        // Notify Explorer that associations changed.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        return ok;
    }

    // Deleting something that is already gone still counts as success.
    bool DeleteRegTree(HKEY root, const std::wstring& subKey)
    {
        const LONG rc = RegDeleteTreeW(root, subKey.c_str());
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND;
    }

    bool DeleteRegValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName)
    {
        const LONG rc = RegDeleteKeyValueW(root, subKey.c_str(), valueName);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND;
    }

    std::wstring ReadRegSzValue(HKEY root, const std::wstring& subKey, const wchar_t* valueNameOrNull)
    {
        wchar_t buffer[512] {};
        DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
        DWORD type = 0;
        const LONG rc = RegGetValueW(
            root,
            subKey.c_str(),
            valueNameOrNull,
            RRF_RT_REG_SZ,
            &type,
            buffer,
            &bytes);
        if (rc != ERROR_SUCCESS)
        {
            return {};
        }
        return buffer;
    }

    bool UnregisterPerUserFileAssociations(const std::wstring& exePath, const std::vector<std::wstring>& extensions)
    {
        const std::wstring appName = L"FICture2";
        const std::wstring progId = L"FICture2.Image";
        const std::wstring exeName = std::filesystem::path(exePath).filename().wstring();
        bool ok = true;

        // Direct extension mapping: only clear (Default) when it still points at us.
        for (const auto& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
            {
                continue;
            }

            const std::wstring extKey = L"Software\\Classes\\" + ext;
            if (_wcsicmp(ReadRegSzValue(HKEY_CURRENT_USER, extKey, nullptr).c_str(), progId.c_str()) == 0)
            {
                ok = ok && DeleteRegValue(HKEY_CURRENT_USER, extKey, nullptr);
            }
            ok = ok && DeleteRegTree(HKEY_CURRENT_USER, extKey + L"\\OpenWithProgids\\" + progId);
        }

        if (!exeName.empty())
        {
            ok = ok && DeleteRegTree(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName);
        }

        ok = ok && DeleteRegValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", appName.c_str());
        ok = ok && DeleteRegTree(HKEY_CURRENT_USER, L"Software\\" + appName + L"\\Capabilities");
        // Only remove the Capabilities subtree; leave any future app keys intact.
        // If Capabilities was the only child, prune the empty FICture2 key best-effort.
        (void)RegDeleteKeyW(HKEY_CURRENT_USER, (L"Software\\" + appName).c_str());

        ok = ok && DeleteRegTree(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId);

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return ok;
    }

    void WriteIniInt(const std::wstring& iniFile, const wchar_t* section, const wchar_t* key, int value)
    {
        if (iniFile.empty() || section == nullptr || key == nullptr)
        {
            return;
        }

        IniStore::SetInt(iniFile, section, key, value);
    }

    void ClampRectToMonitorWorkArea(RECT& rc, bool enableLog = false)
    {
        // If the rect is invalid, bail.
        if (rc.right <= rc.left || rc.bottom <= rc.top)
        {
            return;
        }

        FIC2_TIMER_START(t_clamp);
        const HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
        if (enableLog) FIC2_LOG_STEP(t_clamp, "[LoadWP]   MonitorFromRect");

        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
        {
            return;
        }
        if (enableLog) FIC2_LOG_STEP(t_clamp, "[LoadWP]   GetMonitorInfoW");

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

#if !FICTURE2_ENABLE_FIRST_RUN_PROMPTS
        // For winget/Store builds: just create INI file and skip all prompts
        EnsureIniFileExists(iniFile, false);
        return;
#else
        // For standalone builds: show prompts on first run
        
        // Thumbnail provider prompt
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

        // Create the INI so we don't ask again on the next run
        EnsureIniFileExists(iniFile, false);
#endif
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
        const std::vector<std::wstring> extsForPrompt = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        const std::wstring extLine = BuildExtensionPromptLine(extsForPrompt);
        const std::wstring msg =
            L"Register FICture2 as the default image viewer for supported types?\n"
            L"(This will configure per-user associations.)\n\n"
            L"Extensions:\n" +
            extLine +
            L"\n\nDo you want to apply this now?";
#else
        const std::vector<std::wstring> extsForPrompt = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        const std::wstring extLine = BuildExtensionPromptLine(extsForPrompt);
        const std::wstring msg =
            L"Register FICture2 capabilities with Windows.\n\n"
            L"After registration, you can set FICture2 as default for image files in:\n"
            L"Windows Settings > Apps > Default apps\n\n"
            L"Extensions:\n" +
            extLine +
            L"\n\nDo you want to register now?";
#endif

        const int choice = MessageBoxW(
            owner,
            msg.c_str(),
            L"FICture2 - File Associations",
            MB_ICONQUESTION | MB_YESNO);
        if (choice != IDYES)
        {
            return false;
        }

        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

        const std::vector<std::wstring> exts = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
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
        // Standalone/Nexus: registration is complete, but Windows may still require explicit default selection.
        MessageBoxW(
            owner,
            L"File associations were registered successfully.\n\n"
            L"If some extensions are still not opening with FICture2,\n"
            L"please set FICture2 in Windows Settings > Apps > Default apps.",
            L"FICture2",
            MB_OK | MB_ICONINFORMATION);

        (void)ShellExecuteW(
            owner,
            L"open",
            L"ms-settings:defaultapps",
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
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

    bool UnregisterSupportedFileAssociations(HWND owner)
    {
        const int choice = MessageBoxW(
            owner,
            L"Remove the per-user file associations for FICture2?\n"
            L"(Explorer will fall back to whatever handler is registered next.)\n\n"
            L"Do you want to remove them now?",
            L"FICture2 - File Associations",
            MB_ICONQUESTION | MB_YESNO);
        if (choice != IDYES)
        {
            return false;
        }

        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

        const std::vector<std::wstring> exts = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        const bool removed = UnregisterPerUserFileAssociations(exePath, exts);

        const std::wstring iniFile = GetIniFilePath();
        if (!iniFile.empty())
        {
            WriteIniInt(iniFile, L"General", L"AssociationsEnabled", 0);
        }

        if (!removed)
        {
            MessageBoxW(
                owner,
                L"Failed to fully remove file associations.\n\n"
                L"Some entries may remain; you can clear them in Windows Settings > Apps > Default apps.",
                L"FICture2",
                MB_OK | MB_ICONWARNING);
            return false;
        }

        MessageBoxW(
            owner,
            L"Per-user file associations were removed.",
            L"FICture2",
            MB_OK | MB_ICONINFORMATION);
        return true;
    }

    bool AreFileAssociationsRegistered()
    {
        HKEY key = nullptr;
        const std::wstring progIdKey = L"Software\\Classes\\FICture2.Image";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, progIdKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return false;
        }
        RegCloseKey(key);
        return true;
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

    bool ReadWindowPlacement(RECT& outRect, int& outShowCmd)
    {
        const std::wstring iniFile = GetIniFilePath();
        if (iniFile.empty())
        {
            return false;
        }

        const auto ini = IniStore::Load(iniFile);
        if (!ini.IsLoaded())
        {
            return false;
        }

        const std::wstring leftStr   = ini.GetString(L"Window", L"Left");
        const std::wstring topStr    = ini.GetString(L"Window", L"Top");
        const std::wstring rightStr  = ini.GetString(L"Window", L"Right");
        const std::wstring bottomStr = ini.GetString(L"Window", L"Bottom");
        const std::wstring showStr   = ini.GetString(L"Window", L"ShowCmd");

        if (leftStr.empty() || topStr.empty() || rightStr.empty() ||
            bottomStr.empty() || showStr.empty())
        {
            return false;
        }

        const auto left = CommonUtil::TryParseInt(leftStr);
        const auto top = CommonUtil::TryParseInt(topStr);
        const auto right = CommonUtil::TryParseInt(rightStr);
        const auto bottom = CommonUtil::TryParseInt(bottomStr);
        const auto show = CommonUtil::TryParseInt(showStr);
        if (!left || !top || !right || !bottom || !show)
        {
            return false;
        }

        RECT rc { *left, *top, *right, *bottom };

        if (rc.right <= rc.left || rc.bottom <= rc.top)
        {
            return false;
        }

        ClampRectToMonitorWorkArea(rc);

        outRect    = rc;
        outShowCmd = *show;
        return true;
    }

    void LoadWindowPlacement(HWND hwnd)
    {
        // This function is kept for fallback use only.
        // Normal startup applies placement via CreateWindowed + Show(showCmd)
        // to avoid the ~50 ms SetWindowPlacement DWM IPC cost.
        if (hwnd == nullptr)
        {
            return;
        }

        RECT rc {};
        int showCmd = 0;
        if (!ReadWindowPlacement(rc, showCmd))
        {
            return;
        }

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

