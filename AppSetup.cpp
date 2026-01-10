#include "AppSetup.h"

#include "framework.h"

#include <filesystem>
#include <vector>

#include <shlobj.h>
#include <shellapi.h>
#include <winreg.h>

namespace
{
    void EnsureIniFileExists(const std::wstring& iniFile, bool associationsEnabled)
    {
        if (iniFile.empty())
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

        // Create a minimal INI so next run isn't treated as "first run".
        (void)WritePrivateProfileStringW(L"General", L"Initialized", L"1", iniFile.c_str());
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
            L"FICture2를 기본 이미지 뷰어로 설정할까요?\n"
            L"(시스템 전체가 아닌, 현재 사용자 계정(HKCU) 기준으로만 설정합니다.)\n\n"
            L"연결할 확장자:\n"
            L".png .jpg .jpeg .bmp .tif .tiff .gif .dds .tga\n\n"
            L"지금 설정하시겠습니까?\n"
            L"\n"
            L"--- English ---\n"
            L"Set FICture2 as your default image viewer?\n"
            L"(This will configure per-user (HKCU) associations only, not system-wide.)\n\n"
            L"Extensions:\n"
            L".png .jpg .jpeg .bmp .tif .tiff .gif .dds .tga\n\n"
            L"Do you want to apply this now?";

        const int choice = MessageBoxW(nullptr, msg, L"FICture2 - 파일 연결", MB_ICONQUESTION | MB_YESNO);

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
                    L"파일 연결 설정에 실패했습니다.\n\n"
                    L"Windows 정책/버전에 따라 앱이 기본 앱으로 자동 설정되지 않을 수 있습니다.\n"
                    L"필요하면 Windows 설정 > 기본 앱에서 수동으로 설정해 주세요.",
                    L"FICture2",
                    MB_OK | MB_ICONWARNING);
            }
        }

        // Create the INI so we don't ask again on the next run.
        EnsureIniFileExists(iniFile, enabled);
    }
}

