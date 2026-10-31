#include "AppDialogs.h"

#include "CommonUtil.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "Resource.h"
#include "Version.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <filesystem>
#include <type_traits>
#include <iterator>
#include <sstream>

namespace
{
    struct IconDeleter
    {
        void operator()(HICON icon) const
        {
            DestroyIcon(icon);
        }
    };
    using UniqueIcon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;

    class NativeDialogs final : public AppDialogs
    {
    public:
        bool PickImageFile(HWND owner, const std::wstring& initialDir, const wchar_t* title, std::wstring& outPath) override
        {
            wchar_t fileName[MAX_PATH] {};
            const std::wstring filter =
                SupportedImageFilter(ImageCore::ImageDecodeDispatcher::GetSupportedExtensions());

            OPENFILENAMEW ofn {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = owner;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.lpstrFilter = filter.c_str();
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
            ofn.lpstrTitle = (title != nullptr) ? title : L"Open Image";
            ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();

            if (!GetOpenFileNameW(&ofn))
            {
                return false;
            }

            const std::filesystem::path chosen { fileName };
            if (!std::filesystem::exists(chosen) || !std::filesystem::is_regular_file(chosen))
            {
                return false;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(chosen.wstring()))
            {
                MessageBoxW(owner, L"Selected file type is not supported.", L"FICture2", MB_OK | MB_ICONWARNING);
                return false;
            }

            outPath = chosen.wstring();
            return true;
        }

        bool PickFolder(HWND owner, std::wstring& outFolder) override
        {
            BROWSEINFOW bi {};
            bi.hwndOwner = owner;
            bi.lpszTitle = L"Select Folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI | BIF_NONEWFOLDERBUTTON;

            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl == nullptr)
            {
                return false;
            }

            wchar_t selectedPath[MAX_PATH] {};
            const BOOL ok = SHGetPathFromIDListW(pidl, selectedPath);
            CoTaskMemFree(pidl);
            if (!ok)
            {
                return false;
            }

            const std::filesystem::path folder { selectedPath };
            if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
            {
                return false;
            }

            outFolder = folder.wstring();
            return true;
        }

        bool PickColor(HWND owner, const D2D1_COLOR_F& current, D2D1_COLOR_F& outColor) override
        {
            static COLORREF s_custom[16] {};

            CHOOSECOLORW cc {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = owner;
            cc.lpCustColors = s_custom;
            cc.rgbResult = RGB(
                CommonUtil::ToByte255(current.r),
                CommonUtil::ToByte255(current.g),
                CommonUtil::ToByte255(current.b));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!ChooseColorW(&cc))
            {
                return false;
            }

            outColor = D2D1::ColorF(
                static_cast<float>(GetRValue(cc.rgbResult)) / 255.0f,
                static_cast<float>(GetGValue(cc.rgbResult)) / 255.0f,
                static_cast<float>(GetBValue(cc.rgbResult)) / 255.0f,
                1.0f);
            return true;
        }

        void ShowAbout(HWND owner) override
        {
            INITCOMMONCONTROLSEX icc {};
            icc.dwSize = sizeof(icc);
            icc.dwICC = ICC_STANDARD_CLASSES;
            InitCommonControlsEx(&icc);

            (void)ShowAboutWith(owner, [](const TASKDIALOGCONFIG& config)
            {
                return TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
            });
        }
    };
}

HRESULT AppDialogs::ShowAboutWith(HWND owner, const ShowTaskDialog& show)
{
    // Loaded without LR_SHARED, so this call owns (and releases) the icon.
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const UniqueIcon appIcon { static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_FICTURE2), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE)) };

    // Plain text version without hyperlinks to avoid network connection attempts
    const wchar_t* taskDialogContent =
        L"Version: " FICTURE2_VERSION_STRING_W L" (" FICTURE2_BUILD_FLAVOR_W L")\n"
        L"Author: floyd Lee (floydles@gmail.com)\n"
        L"GitHub: https://github.com/floyd68/FICture2";

    TASKDIALOGCONFIG config {};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN;
    config.pszWindowTitle = L"About FICture2";
    config.pszMainInstruction = L"FICture2";
    config.pszContent = taskDialogContent;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.hMainIcon = appIcon.get();

    return show(config);
}

std::shared_ptr<AppDialogs> AppDialogs::Native()
{
    return std::make_shared<NativeDialogs>();
}

std::wstring AppDialogs::SupportedImageFilter(const std::vector<std::wstring>& extensions)
{
    std::wostringstream patternBuilder {};
    for (size_t i = 0; i < extensions.size(); ++i)
    {
        if (i != 0)
        {
            patternBuilder << L";";
        }
        patternBuilder << L"*" << extensions[i];
    }

    std::wstring wildcardPattern = patternBuilder.str();
    if (wildcardPattern.empty())
    {
        wildcardPattern = L"*.*";
    }

    std::wstring filter {};
    filter.reserve(256 + wildcardPattern.size() * 2);
    filter += L"Supported images (" + wildcardPattern + L")";
    filter.push_back(L'\0');
    filter += wildcardPattern;
    filter.push_back(L'\0');
    filter += L"All files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

std::wstring AppDialogs::InitialDir(const std::wstring& folder)
{
    const std::filesystem::path candidate { folder };
    std::error_code ec;
    if (!candidate.empty() && std::filesystem::is_directory(candidate, ec))
    {
        return candidate.wstring();
    }
    return L"";
}
