#include "Ficture2Backplate.h"

#include "AppLog.h"
#include "CommonUtil.h"
#include "IniStore.h"
#include "FD2D/Core.h"
#include "AppSetup.h"
#include "Resource.h"
#include "Version.h"
#include "ImageBrowser.h"
#include "ImageBrowserContextMenu.h"
#include "ImageBrowserSessionPersistence.h"
#include "ImageBrowserSplitCoordinator.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "ImageAwareVfs.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"
#include "RecentFiles.h"
#include "ScreenshotUtil.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <sstream>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

namespace
{
    bool TryGetIniFilePath(std::wstring& outIniFile)
    {
        outIniFile = FICture2App::GetIniFilePath();
        return !outIniFile.empty();
    }

    bool TryParseRgb(const wchar_t* text, D2D1_COLOR_F& outColor)
    {
        if (text == nullptr || *text == 0)
        {
            return false;
        }

        int r = -1;
        int g = -1;
        int b = -1;
        if (swscanf_s(text, L"%d,%d,%d", &r, &g, &b) != 3)
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

    void WriteRgbToIni(const std::wstring& iniFile, const wchar_t* section, const wchar_t* key, const D2D1_COLOR_F& color)
    {
        if (iniFile.empty() || section == nullptr || key == nullptr)
        {
            return;
        }

        const std::wstring rgb = std::format(
            L"{},{},{}",
            CommonUtil::ToByte255(color.r),
            CommonUtil::ToByte255(color.g),
            CommonUtil::ToByte255(color.b));
        IniStore::SetString(iniFile, section, key, rgb);
    }

    template <typename T>
    T* AsImageBrowser(FD2D::Wnd* wnd)
    {
        if (wnd == nullptr)
        {
            return nullptr;
        }

        return dynamic_cast<T*>(wnd);
    }

    IImageBrowserOps* AsImageBrowserOps(FD2D::Wnd* wnd)
    {
        return AsImageBrowser<IImageBrowserOps>(wnd);
    }

    IImageBrowserSync* AsImageBrowserSync(FD2D::Wnd* wnd)
    {
        return AsImageBrowser<IImageBrowserSync>(wnd);
    }

    IImageBrowserQuery* AsImageBrowserQuery(FD2D::Wnd* wnd)
    {
        return AsImageBrowser<IImageBrowserQuery>(wnd);
    }

    template <typename T>
    std::vector<T*> SnapshotBrowsers(
        const std::shared_ptr<Ficture2Backplate::EventBus>& bus,
        FD2D::Wnd* exclude = nullptr)
    {
        std::vector<T*> out;
        if (!bus)
        {
            return out;
        }

        const auto browsers = bus->ImageBrowsersSnapshot();
        out.reserve(browsers.size());
        for (auto* browser : browsers)
        {
            if (browser == nullptr || browser == exclude)
            {
                continue;
            }

            auto* target = AsImageBrowser<T>(browser);
            if (target != nullptr)
            {
                out.push_back(target);
            }
        }

        return out;
    }

    std::vector<IImageBrowserOps*> SnapshotBrowserOps(const std::shared_ptr<Ficture2Backplate::EventBus>& bus)
    {
        return SnapshotBrowsers<IImageBrowserOps>(bus);
    }

    std::vector<IImageBrowserSync*> SnapshotOtherBrowserSync(
        const std::shared_ptr<Ficture2Backplate::EventBus>& bus,
        FD2D::Wnd* source)
    {
        return SnapshotBrowsers<IImageBrowserSync>(bus, source);
    }

    std::vector<IImageBrowserOps*> SnapshotOtherBrowserOps(
        const std::shared_ptr<Ficture2Backplate::EventBus>& bus,
        FD2D::Wnd* source)
    {
        return SnapshotBrowsers<IImageBrowserOps>(bus, source);
    }

    void ShowAboutDialog(HWND hwnd)
    {
        INITCOMMONCONTROLSEX icc {};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HICON appIcon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(IDI_FICTURE2), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));

        // Plain text version without hyperlinks to avoid network connection attempts
        const wchar_t* taskDialogContent =
            L"Version: " FICTURE2_VERSION_STRING_W L" (" FICTURE2_BUILD_FLAVOR_W L")\n"
            L"Author: floyd Lee (floydles@gmail.com)\n"
            L"GitHub: https://github.com/floyd68/FICture2";

        TASKDIALOGCONFIG config {};
        config.cbSize = sizeof(config);
        config.hwndParent = hwnd;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN;
        config.pszWindowTitle = L"About FICture2";
        config.pszMainInstruction = L"FICture2";
        config.pszContent = taskDialogContent;
        config.dwCommonButtons = TDCBF_OK_BUTTON;
        config.hMainIcon = appIcon;

        TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    }

    enum class BrowserCommandAction
    {
        OpenImage,
        OpenFolder,
        OpenImageSplitNew,
        OpenNewImage,
        Close,
        ActivateSelected,
        SelectPrevious,
        SelectNext,
        SelectFirst,
        SelectLast,
        PagePrevious,
        PageNext,
        NavigateUp,
        FitToScreen,
        BackgroundColor,
        FocusedBackgroundColor,
        ToggleDirectories,
        ToggleAlpha,
        ToggleSampling,
        RotateLeft,
        RotateRight,
        Rotate180,
        RotateReset,
        ShowInExplorerAtPoint,
        ShowAbout,
        SaveScreenshot,
        ClearRecent,
        RegisterAssociations,
        UnregisterAssociations,
        RegisterThumbnailProvider,
        UnregisterThumbnailProvider
    };

    struct CommandMapEntry
    {
        UINT id { 0 };
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
    };

    struct KeyChordMapEntry
    {
        UINT key { 0 };
        bool ctrl { false };
        bool shift { false };
        bool alt { false };
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
    };

    struct BrowserOpsActionEntry
    {
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
        void (IImageBrowserOps::*invoke)() { nullptr };
    };

    // Centralized command/key policy maps.
    // Keep this section as the single source of truth for user-facing command routing.

    // Context menu: file/viewer lifecycle commands.
    static const CommandMapEntry kContextLifecycleMap[] =
    {
        { IDM_CTX_OPEN_IMAGE, BrowserCommandAction::OpenImage },
        { IDM_CTX_OPEN_FOLDER, BrowserCommandAction::OpenFolder },
        { IDM_CTX_OPEN_NEW_IMAGE, BrowserCommandAction::OpenNewImage },
        { IDM_CTX_CLOSE, BrowserCommandAction::Close },
    };

    // Context menu: view/appearance commands.
    static const CommandMapEntry kContextViewAppearanceMap[] =
    {
        { IDM_CTX_FIT_TO_SCREEN, BrowserCommandAction::FitToScreen },
        { IDM_CTX_BACKGROUND_COLOR, BrowserCommandAction::BackgroundColor },
        { IDM_CTX_FOCUSED_BACKGROUND_COLOR, BrowserCommandAction::FocusedBackgroundColor },
        { IDM_CTX_TOGGLE_DIRECTORIES, BrowserCommandAction::ToggleDirectories },
        { IDM_CTX_TOGGLE_ALPHA, BrowserCommandAction::ToggleAlpha },
        { IDM_CTX_TOGGLE_SAMPLING, BrowserCommandAction::ToggleSampling },
        { IDM_CTX_ROTATE_LEFT,  BrowserCommandAction::RotateLeft  },
        { IDM_CTX_ROTATE_RIGHT, BrowserCommandAction::RotateRight },
        { IDM_CTX_ROTATE_180,   BrowserCommandAction::Rotate180   },
        { IDM_CTX_ROTATE_RESET, BrowserCommandAction::RotateReset },
    };

    // Context menu: integration/system commands.
    static const CommandMapEntry kContextIntegrationMap[] =
    {
        { IDM_CTX_SHOW_IN_EXPLORER, BrowserCommandAction::ShowInExplorerAtPoint },
        { IDM_CTX_SAVE_SCREENSHOT, BrowserCommandAction::SaveScreenshot },
        { IDM_CTX_CLEAR_RECENT, BrowserCommandAction::ClearRecent },
        { IDM_CTX_ABOUT, BrowserCommandAction::ShowAbout },
    };

#if FICTURE2_ENABLE_REGISTRATION_MENU
    static const CommandMapEntry kContextRegistrationMap[] =
    {
        { IDM_CTX_REGISTER_ASSOCIATIONS, BrowserCommandAction::RegisterAssociations },
        { IDM_CTX_UNREGISTER_ASSOCIATIONS, BrowserCommandAction::UnregisterAssociations },
        { IDM_CTX_REGISTER_THUMBNAIL_PROVIDER, BrowserCommandAction::RegisterThumbnailProvider },
        { IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER, BrowserCommandAction::UnregisterThumbnailProvider },
    };
#endif

    // KeyUp: selection/navigation commands without modifier chords.
    static const CommandMapEntry kKeySelectionNavigationMap[] =
    {
        { VK_RETURN, BrowserCommandAction::ActivateSelected },
        { VK_HOME, BrowserCommandAction::SelectFirst },
        { VK_END, BrowserCommandAction::SelectLast },
        { VK_PRIOR, BrowserCommandAction::PagePrevious },
        { VK_NEXT, BrowserCommandAction::PageNext },
        { VK_BACK, BrowserCommandAction::NavigateUp },
    };

    // KeyDown: immediate left/right selection movement.
    static const CommandMapEntry kKeyDownSelectionNavigationMap[] =
    {
        { VK_LEFT, BrowserCommandAction::SelectPrevious },
        { VK_RIGHT, BrowserCommandAction::SelectNext },
    };

    // KeyUp: lifecycle modifier chords (Ctrl/Ctrl+Shift).
    static const KeyChordMapEntry kKeyLifecycleChordMap[] =
    {
        { VK_F4, true, false, false, BrowserCommandAction::Close },
        { VK_F4, true, true, false, BrowserCommandAction::Close },
        { 'O', true, false, false, BrowserCommandAction::OpenImage },
        { 'O', true, true, false, BrowserCommandAction::OpenImageSplitNew },
    };

    // KeyUp: Alt+* appearance/navigation chords.
    static const KeyChordMapEntry kKeyViewAppearanceChordMap[] =
    {
        { VK_UP, false, false, true, BrowserCommandAction::NavigateUp },
        { 'N', false, false, true, BrowserCommandAction::ToggleDirectories },
        { 'A', false, false, true, BrowserCommandAction::ToggleAlpha },
        { 'X', false, false, true, BrowserCommandAction::FitToScreen },
        { 'B', false, false, true, BrowserCommandAction::BackgroundColor },
        { 'Q', false, false, true, BrowserCommandAction::ToggleSampling },
        // < / > : rotate left / right (Shift+comma / Shift+period)
        { VK_OEM_COMMA,  false, true, false, BrowserCommandAction::RotateLeft  },
        { VK_OEM_PERIOD, false, true, false, BrowserCommandAction::RotateRight },
    };

    // Keep in sync with ImageBrowserDragController's default insert threshold.
    static constexpr float kMultiDropInsertThreshold = 0.75f;

    static const BrowserOpsActionEntry kDirectBrowserOpsActions[] =
    {
        { BrowserCommandAction::Close, &IImageBrowserOps::BrowserCmdClose },
        { BrowserCommandAction::ActivateSelected, &IImageBrowserOps::BrowserCmdActivateSelected },
        { BrowserCommandAction::SelectPrevious, &IImageBrowserOps::BrowserCmdSelectPrevious },
        { BrowserCommandAction::SelectNext, &IImageBrowserOps::BrowserCmdSelectNext },
        { BrowserCommandAction::SelectFirst, &IImageBrowserOps::BrowserCmdSelectFirst },
        { BrowserCommandAction::SelectLast, &IImageBrowserOps::BrowserCmdSelectLast },
        { BrowserCommandAction::PagePrevious, &IImageBrowserOps::BrowserCmdPagePrevious },
        { BrowserCommandAction::PageNext, &IImageBrowserOps::BrowserCmdPageNext },
        { BrowserCommandAction::NavigateUp, &IImageBrowserOps::BrowserCmdNavigateUp },
        { BrowserCommandAction::FitToScreen, &IImageBrowserOps::BrowserCmdFitToScreen },
        { BrowserCommandAction::ToggleDirectories, &IImageBrowserOps::BrowserCmdToggleDirectories },
        { BrowserCommandAction::ToggleAlpha, &IImageBrowserOps::BrowserCmdToggleAlpha },
        { BrowserCommandAction::ToggleSampling, &IImageBrowserOps::BrowserCmdToggleSampling },
        { BrowserCommandAction::RotateLeft,  &IImageBrowserOps::BrowserCmdRotateLeft  },
        { BrowserCommandAction::RotateRight, &IImageBrowserOps::BrowserCmdRotateRight },
        { BrowserCommandAction::Rotate180,   &IImageBrowserOps::BrowserCmdRotate180   },
        { BrowserCommandAction::RotateReset, &IImageBrowserOps::BrowserCmdRotateReset },
    };

    std::wstring BuildSupportedImageDialogFilter()
    {
        const std::vector<std::wstring> exts = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        std::wostringstream patternBuilder {};
        for (size_t i = 0; i < exts.size(); ++i)
        {
            if (i != 0)
            {
                patternBuilder << L";";
            }
            patternBuilder << L"*" << exts[i];
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

    bool TryPickSupportedImageFile(
        HWND ownerWindow,
        const std::wstring& initialDir,
        const wchar_t* title,
        std::wstring& outFilePath)
    {
        wchar_t fileName[MAX_PATH] {};
        const std::wstring filter = BuildSupportedImageDialogFilter();

        OPENFILENAMEW ofn {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = ownerWindow;
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
            MessageBoxW(ownerWindow, L"Selected file type is not supported.", L"FICture2", MB_OK | MB_ICONWARNING);
            return false;
        }

        outFilePath = chosen.wstring();
        return true;
    }

    bool TryPickFolder(HWND ownerWindow, std::wstring& outFolderPath)
    {
        BROWSEINFOW bi {};
        bi.hwndOwner = ownerWindow;
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

        outFolderPath = folder.wstring();
        return true;
    }

    bool TryPickColor(HWND ownerWindow, const D2D1_COLOR_F& current, D2D1_COLOR_F& outColor)
    {
        static COLORREF s_custom[16] {};

        CHOOSECOLORW cc {};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = ownerWindow;
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

    std::wstring ResolveInitialDialogDir(IImageBrowserOps* browserOps)
    {
        if (browserOps == nullptr)
        {
            return L"";
        }

        const std::filesystem::path candidate { browserOps->BrowserGetCurrentFolderPath() };
        if (!candidate.empty() && std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
        {
            return candidate.wstring();
        }

        return L"";
    }

    void ExecuteBrowserCommandAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        IImageBrowserOps* browserOps,
        BrowserCommandAction action,
        const POINT& ptClient,
        HWND ownerWindow);

    bool TryExecuteDirectBrowserOpsAction(IImageBrowserOps* browserOps, BrowserCommandAction action)
    {
        if (browserOps == nullptr)
        {
            return false;
        }

        for (const auto& entry : kDirectBrowserOpsActions)
        {
            if (entry.action == action && entry.invoke != nullptr)
            {
                (browserOps->*entry.invoke)();
                return true;
            }
        }

        return false;
    }

    bool TryExecuteOpenAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        IImageBrowserOps* browserOps,
        BrowserCommandAction action,
        HWND ownerWindow)
    {
        if (backplate == nullptr || source == nullptr || browserOps == nullptr)
        {
            return false;
        }

        if (action != BrowserCommandAction::OpenImage &&
            action != BrowserCommandAction::OpenFolder &&
            action != BrowserCommandAction::OpenImageSplitNew &&
            action != BrowserCommandAction::OpenNewImage)
        {
            return false;
        }

        if (action == BrowserCommandAction::OpenFolder)
        {
            std::wstring selectedFolder {};
            if (!TryPickFolder(ownerWindow, selectedFolder))
            {
                return true;
            }

            browserOps->BrowserRestoreOpenFolder(selectedFolder);
            return true;
        }

        std::wstring selectedPath {};
        const std::wstring initialDir = ResolveInitialDialogDir(browserOps);
        const wchar_t* title = (action == BrowserCommandAction::OpenNewImage) ? L"Open New Image" : L"Open Image";
        if (!TryPickSupportedImageFile(ownerWindow, initialDir, title, selectedPath))
        {
            return true;
        }

        if (action == BrowserCommandAction::OpenImage)
        {
            browserOps->BrowserRestoreOpenFile(selectedPath);
            return true;
        }

        if (action == BrowserCommandAction::OpenImageSplitNew)
        {
            browserOps->BrowserOpenAdditionalFileInHorizontalSplit(selectedPath);
            return true;
        }

        auto* bus = backplate->BusPtr().get();
        if (bus == nullptr || bus->ImageBrowserCount() >= ImageBrowserSplitCoordinator::kMaxViewers)
        {
            return true;
        }

        browserOps->BrowserOpenAdditionalFilesSideBySideAfterName({ selectedPath }, source->Name());
        return true;
    }

    bool TryExecuteBackgroundColorAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        BrowserCommandAction action,
        HWND ownerWindow)
    {
        if (backplate == nullptr || source == nullptr)
        {
            return false;
        }

        if (action != BrowserCommandAction::BackgroundColor &&
            action != BrowserCommandAction::FocusedBackgroundColor)
        {
            return false;
        }

        auto* sourceSync = AsImageBrowserSync(source);
        if (sourceSync == nullptr)
        {
            return true;
        }

        const D2D1_COLOR_F current = (action == BrowserCommandAction::BackgroundColor)
            ? backplate->ClearColor()
            : backplate->FocusedBackgroundColor();
        D2D1_COLOR_F nextColor {};
        if (!TryPickColor(ownerWindow, current, nextColor))
        {
            return true;
        }

        if (action == BrowserCommandAction::BackgroundColor)
        {
            backplate->SetClearColor(nextColor);
            sourceSync->BrowserApplyBackgroundColorForSync(nextColor);
            backplate->SynchronizeBackgroundColor(source, nextColor);
        }
        else
        {
            sourceSync->BrowserApplyFocusedBackgroundColorForSync(nextColor);
            backplate->SynchronizeFocusedBackgroundColor(source, nextColor);
        }
        backplate->Render();
        return true;
    }

    bool TryExecuteShowInExplorerAction(
        IImageBrowserOps* browserOps,
        BrowserCommandAction action,
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (browserOps == nullptr)
        {
            return false;
        }

        if (action != BrowserCommandAction::ShowInExplorerAtPoint)
        {
            return false;
        }

        const std::wstring targetPath = browserOps->BrowserGetExplorerTargetPathAtPoint(ptClient);
        if (targetPath.empty())
        {
            return true;
        }

        const std::wstring args = L"/select,\"" + targetPath + L"\"";
        const HINSTANCE result = ShellExecuteW(ownerWindow, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(result) <= 32)
        {
            const std::filesystem::path parent = std::filesystem::path(targetPath).parent_path();
            if (!parent.empty())
            {
                (void)ShellExecuteW(ownerWindow, L"open", parent.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }

        return true;
    }

    bool TryExecuteSaveScreenshotAction(
        FD2D::Backplate& backplate,
        IImageBrowserOps* browserOps,
        BrowserCommandAction action,
        HWND ownerWindow)
    {
        if (browserOps == nullptr || action != BrowserCommandAction::SaveScreenshot)
        {
            return false;
        }

        D2D1_RECT_F rect {};
        const bool canCapture =
            backplate.D3DDevice() != nullptr &&
            backplate.UseOffscreenBuffer() &&
            browserOps->BrowserTryGetMainImageClientRect(rect);
        if (!canCapture)
        {
            return true;
        }

        const std::wstring displayed = browserOps->BrowserGetDisplayedFilePath();
        std::filesystem::path imagePath = displayed.empty()
            ? std::filesystem::path()
            : std::filesystem::path(displayed);
        const std::wstring folder = imagePath.has_parent_path()
            ? imagePath.parent_path().wstring()
            : std::wstring();
        const std::wstring stem = imagePath.has_stem()
            ? imagePath.stem().wstring()
            : L"ficture2";

        std::wstring name = stem + L"_screenshot1.png";
        for (int n = 1; !folder.empty() && n < 1000; ++n)
        {
            name = stem + L"_screenshot" + std::to_wstring(n) + L".png";
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path(folder) / name, ec))
            {
                break;
            }
        }

        std::wstring outPath;
        if (!ScreenshotUtil::ShowSavePngDialog(ownerWindow, folder, name, outPath))
        {
            return true;
        }

        if (!ScreenshotUtil::SaveRenderSurfaceRectPng(backplate, rect, outPath))
        {
            FIC2_LOG_ERROR("[Screenshot] Save failed: {}", std::filesystem::path(outPath).string());
            MessageBoxW(
                ownerWindow,
                (L"Failed to save screenshot:\n" + outPath).c_str(),
                L"FICture2",
                MB_OK | MB_ICONERROR);
        }

        return true;
    }

    bool TryExecuteClearRecentAction(BrowserCommandAction action)
    {
        if (action != BrowserCommandAction::ClearRecent)
        {
            return false;
        }

        RecentFiles::Clear(FICture2App::GetIniFilePath());
        return true;
    }

    bool TryExecuteSystemAction(BrowserCommandAction action, HWND ownerWindow)
    {
        if (action == BrowserCommandAction::ShowAbout)
        {
            ShowAboutDialog(ownerWindow);
            return true;
        }

#if FICTURE2_ENABLE_REGISTRATION_MENU
        if (action == BrowserCommandAction::RegisterAssociations)
        {
            FICture2App::RegisterSupportedFileAssociations(ownerWindow);
            return true;
        }

        if (action == BrowserCommandAction::UnregisterAssociations)
        {
            FICture2App::UnregisterSupportedFileAssociations(ownerWindow);
            return true;
        }

        if (action == BrowserCommandAction::RegisterThumbnailProvider ||
            action == BrowserCommandAction::UnregisterThumbnailProvider)
        {
            FICture2App::RegisterThumbnailProvider(
                ownerWindow,
                action == BrowserCommandAction::UnregisterThumbnailProvider);
            return true;
        }
#endif

        return false;
    }

    template <size_t N>
    bool TryDispatchMappedAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        IImageBrowserOps* browserOps,
        UINT id,
        const CommandMapEntry (&entries)[N],
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (browserOps == nullptr)
        {
            return false;
        }

        for (const auto& entry : entries)
        {
            if (entry.id == id)
            {
                ExecuteBrowserCommandAction(backplate, source, browserOps, entry.action, ptClient, ownerWindow);
                return true;
            }
        }

        return false;
    }

    template <size_t N>
    bool TryDispatchMappedKeyAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        IImageBrowserOps* browserOps,
        UINT key,
        bool ctrl,
        bool shift,
        bool alt,
        const KeyChordMapEntry (&entries)[N],
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (browserOps == nullptr)
        {
            return false;
        }

        for (const auto& entry : entries)
        {
            if (entry.key == key &&
                entry.ctrl == ctrl &&
                entry.shift == shift &&
                entry.alt == alt)
            {
                ExecuteBrowserCommandAction(backplate, source, browserOps, entry.action, ptClient, ownerWindow);
                return true;
            }
        }

        return false;
    }

    void ExecuteBrowserCommandAction(
        Ficture2Backplate* backplate,
        FD2D::Wnd* source,
        IImageBrowserOps* browserOps,
        BrowserCommandAction action,
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (browserOps == nullptr || backplate == nullptr || source == nullptr)
        {
            return;
        }

        if (TryExecuteDirectBrowserOpsAction(browserOps, action))
        {
            return;
        }

        if (TryExecuteOpenAction(backplate, source, browserOps, action, ownerWindow))
        {
            return;
        }

        if (TryExecuteBackgroundColorAction(backplate, source, action, ownerWindow))
        {
            return;
        }

        if (TryExecuteShowInExplorerAction(browserOps, action, ptClient, ownerWindow))
        {
            return;
        }

        if (TryExecuteSaveScreenshotAction(*backplate, browserOps, action, ownerWindow))
        {
            return;
        }

        if (TryExecuteClearRecentAction(action))
        {
            return;
        }

        (void)TryExecuteSystemAction(action, ownerWindow);
    }

}

Ficture2Backplate::EventBus::HandlerId Ficture2Backplate::EventBus::Subscribe(Handler handler)
{
    if (!handler)
    {
        return 0;
    }

    const HandlerId id = m_nextId++;
    m_handlers.emplace(id, std::move(handler));
    return id;
}

void Ficture2Backplate::EventBus::Unsubscribe(HandlerId id)
{
    m_handlers.erase(id);
}

void Ficture2Backplate::EventBus::Publish(const ImageBrowserEvent& event)
{
    for (const auto& pair : m_handlers)
    {
        if (pair.second)
            pair.second(event);
    }
}

void Ficture2Backplate::EventBus::RegisterImageBrowser(FD2D::Wnd* browser)
{
    if (browser == nullptr)
        return;

    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
        return;

    m_imageBrowsers.push_back(browser);
}

void Ficture2Backplate::EventBus::UnregisterImageBrowser(FD2D::Wnd* browser)
{
    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
        m_imageBrowsers.erase(it);
}

FD2D::Wnd* Ficture2Backplate::FindTargetWnd(const POINT& ptClient)
{
    if (!m_eventBus)
    {
        return nullptr;
    }

    FD2D::Wnd* best = nullptr;
    float bestArea = 0.0f;

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    for (auto* b : browsers)
    {
        if (b == nullptr)
        {
            continue;
        }

        const D2D1_RECT_F r = b->LayoutRect();
        if (ptClient.x < r.left || ptClient.x > r.right || ptClient.y < r.top || ptClient.y > r.bottom)
        {
            continue;
        }

        const float w = (std::max)(0.0f, r.right - r.left);
        const float h = (std::max)(0.0f, r.bottom - r.top);
        const float area = w * h;

        // Choose the smallest containing rect (most specific/deepest pane).
        if (best == nullptr || area < bestArea)
        {
            best = b;
            bestArea = area;
        }
    }

    return best;
}

bool Ficture2Backplate::HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient)
{
    if (paths.empty())
    {
        return false;
    }

    std::vector<std::wstring> supported;
    supported.reserve(paths.size());
    for (const auto& path : paths)
    {
        auto vp = Floar::VirtualPath::Parse(path);
        if (!vp)
        {
            continue;
        }

        if (ImageAwareVfs::IsBrowsableDropTarget(*vp))
        {
            supported.push_back(path);
        }
    }

    if (supported.empty())
    {
        return false;
    }

    if (supported.size() > 1)
    {
        FD2D::Wnd* target = FindTargetWnd(ptClient);
        const std::wstring targetName = target ? target->Name() : L"";
        bool insertMode = false;

        if (target != nullptr)
        {
            const D2D1_RECT_F r = target->LayoutRect();
            const float w = (std::max)(1.0f, r.right - r.left);
            const float relX = (static_cast<float>(ptClient.x) - r.left) / w;
            insertMode = (relX >= kMultiDropInsertThreshold);
        }

        if (insertMode)
        {
            OpenAdditionalFilesSideBySideAfter(supported, targetName);
            return true;
        }

        bool handled = false;
        if (target != nullptr)
        {
            handled = target->OnFileDrop(supported.front(), ptClient);
        }

        if (!handled)
        {
            // Route to UI tree: hit-test top-level children and allow Wnd overrides to handle.
            for (auto& pair : m_children)
            {
                if (pair.second && pair.second->OnFileDrop(supported.front(), ptClient))
                {
                    handled = true;
                    break;
                }
            }
        }

        std::vector<std::wstring> remaining(supported.begin() + 1, supported.end());
        OpenAdditionalFilesSideBySideAfter(remaining, targetName);
        return true;
    }

    const std::wstring& path = supported.front();

    // Route to UI tree: hit-test top-level children and allow Wnd overrides to handle.
    for (auto& pair : m_children)
    {
        if (pair.second && pair.second->OnFileDrop(path, ptClient))
        {
            return true;
        }
    }

    return false;
}

bool Ficture2Backplate::HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    UNREFERENCED_PARAMETER(hWnd);

    switch (message)
    {
    case WM_CREATE:
    {
        UpdateTitleBarInfo();
        DragAcceptFiles(m_window, TRUE);
        (void)EnsureDropTargetRegistered();
        result = 0;
        return true;
    }
    case WM_DROPFILES:
    {
        const HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        if (hDrop == nullptr)
        {
            result = 0;
            return true;
        }

        wchar_t pathBuf[MAX_PATH] {};
        const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (fileCount == 0)
        {
            DragFinish(hDrop);
            result = 0;
            return true;
        }

        POINT pt {};
        (void)DragQueryPoint(hDrop, &pt); // client coordinates

        std::vector<std::wstring> dropped;
        dropped.reserve(fileCount);
        for (UINT i = 0; i < fileCount; ++i)
        {
            const UINT cch = DragQueryFileW(hDrop, i, pathBuf, static_cast<UINT>(std::size(pathBuf)));
            if (cch == 0)
            {
                continue;
            }
            dropped.emplace_back(pathBuf);
        }

        DragFinish(hDrop);

        if (dropped.empty())
        {
            result = 0;
            return true;
        }

        (void)HandleFileDropPaths(dropped, pt);

        result = 0;
        return true;
    }
    case WM_DESTROY:
    {
        HandleFileDragLeave();
        UnregisterDropTarget();
        break;
    }
    default:
        break;
    }

    return Backplate::HandleMessage(hWnd, message, wParam, lParam, result);
}

void Ficture2Backplate::EnsureImageBrowserIniInitialized()
{
    if (m_imageBrowserIniInitialized)
    {
        return;
    }
    m_imageBrowserIniInitialized = true;

    std::wstring iniFile {};
    if (!TryGetIniFilePath(iniFile))
    {
        return;
    }

    // Read the entire INI file once; avoid calling GetPrivateProfile* APIs
    // which re-open and re-parse the file on every individual call.
    FIC2_TIMER_START(t_ini);
    const auto ini = IniStore::Load(iniFile);
    FIC2_LOG_STEP(t_ini, "[IniInit] IniStore::Load");
    if (!ini.IsLoaded())
    {
        return;
    }

    m_showNavItems = (ini.GetInt(L"Viewer", L"ShowNavItems", 1) != 0);
    FIC2_LOG_STEP(t_ini, "[IniInit] key reads + SetClearColor");

    const std::wstring bgColor = ini.GetString(L"Window", L"BackgroundColor");
    if (!bgColor.empty())
    {
        D2D1_COLOR_F color {};
        if (TryParseRgb(bgColor.c_str(), color))
        {
            SetClearColor(color);
        }
    }

    const std::wstring focusedColor = ini.GetString(L"Window", L"FocusedBackgroundColor");
    if (!focusedColor.empty())
    {
        D2D1_COLOR_F fc {};
        if (TryParseRgb(focusedColor.c_str(), fc))
        {
            m_focusedBackgroundColor = fc;
        }
    }

    const float zoomStiffness = ini.GetFloat(L"Image", L"ZoomStiffness", 80.0f);
    if (zoomStiffness >= 10.0f && zoomStiffness <= 500.0f)
    {
        m_imageZoomStiffness = zoomStiffness;
    }
}

void Ficture2Backplate::SetSyncedThumbStripHeight(float height)
{
    m_syncedThumbStripHeight = height;
    m_hasSyncedThumbStripHeight = true;
}

bool Ficture2Backplate::TryGetSyncedThumbStripHeight(float& outHeight) const
{
    if (!m_hasSyncedThumbStripHeight)
    {
        return false;
    }

    outHeight = m_syncedThumbStripHeight;
    return true;
}

void Ficture2Backplate::SynchronizeThumbStripHeight(FD2D::Wnd* source, float height)
{
    if (!m_eventBus || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    SetSyncedThumbStripHeight(height);
    if (m_eventBus->ImageBrowserCount() < 2)
    {
        return;
    }

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserForceApplySyncedThumbStripHeight();
        }
    }
}

void Ficture2Backplate::SynchronizeFileSelection(FD2D::Wnd* source, const std::wstring& fileNameLower)
{
    if (!m_eventBus || source == nullptr || fileNameLower.empty())
    {
        return;
    }

    if (m_eventBus->ImageBrowserCount() < 2)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    auto* sourceQuery = AsImageBrowserQuery(source);
    if (sourceSync == nullptr || sourceQuery == nullptr || !sourceQuery->BrowserHasFocusForTitle())
    {
        return;
    }
    (void)sourceSync;

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserSelectFileNameForSync(fileNameLower);
        }
    }
}

void Ficture2Backplate::SynchronizeViewTransform(
    FD2D::Wnd* source,
    const std::wstring& fileNameLower,
    const ImageViewTransform& viewTransform)
{
    if (!m_eventBus || source == nullptr || fileNameLower.empty())
    {
        return;
    }

    if (m_eventBus->ImageBrowserCount() < 2)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    auto* sourceQuery = AsImageBrowserQuery(source);
    if (sourceSync == nullptr || sourceQuery == nullptr || !sourceQuery->BrowserHasFocusForTitle())
    {
        return;
    }
    (void)sourceSync;

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target == nullptr)
        {
            continue;
        }

        const std::wstring targetNameLower = target->BrowserGetActiveFileNameLower();
        if (!targetNameLower.empty() && targetNameLower == fileNameLower)
        {
            target->BrowserApplyViewTransformForSync(viewTransform);
        }
    }
}

void Ficture2Backplate::SynchronizeShowNavItems(FD2D::Wnd* source, bool showNavItems)
{
    if (!m_eventBus || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    EnsureImageBrowserIniInitialized();
    m_showNavItems = showNavItems;

    std::wstring iniFile {};
    if (TryGetIniFilePath(iniFile))
    {
        IniStore::SetString(iniFile, L"Viewer", L"ShowNavItems", m_showNavItems ? L"1" : L"0");
    }

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserApplyShowNavItemsForSync(showNavItems);
        }
    }
}

void Ficture2Backplate::SynchronizeBackgroundColor(FD2D::Wnd* source, const D2D1_COLOR_F& color)
{
    if (!m_eventBus || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    EnsureImageBrowserIniInitialized();
    SetClearColor(color);

    std::wstring iniFile {};
    if (TryGetIniFilePath(iniFile))
    {
        WriteRgbToIni(iniFile, L"Window", L"BackgroundColor", color);
    }

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserApplyBackgroundColorForSync(color);
        }
    }
}

void Ficture2Backplate::SynchronizeFocusedBackgroundColor(FD2D::Wnd* source, const D2D1_COLOR_F& color)
{
    if (!m_eventBus || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    EnsureImageBrowserIniInitialized();
    m_focusedBackgroundColor = color;

    std::wstring iniFile {};
    if (TryGetIniFilePath(iniFile))
    {
        WriteRgbToIni(iniFile, L"Window", L"FocusedBackgroundColor", color);
    }

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserApplyFocusedBackgroundColorForSync(color);
        }
    }
}

void Ficture2Backplate::SynchronizeAlphaCheckerboard(FD2D::Wnd* source, bool checkerEnabled)
{
    if (!m_eventBus || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    m_alphaCheckerboardEnabled = checkerEnabled;

    const auto targets = SnapshotOtherBrowserSync(m_eventBus, source);
    for (auto* target : targets)
    {
        if (target != nullptr)
        {
            target->BrowserApplyAlphaCheckerboardForSync(checkerEnabled);
        }
    }
}

bool Ficture2Backplate::TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath)
{
    if (!m_eventBus || incomingFilePath.empty())
    {
        return false;
    }

    const std::wstring incomingName = IpcOpenQueue::FileNameLower(incomingFilePath);
    if (incomingName.empty())
    {
        return false;
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps == nullptr)
        {
            continue;
        }

        if (browserOps->BrowserGetActiveFileNameLower() != incomingName)
        {
            continue;
        }

        // Name matches an open pane - open the incoming path as a new horizontal viewer.
        browserOps->BrowserOpenAdditionalFileInHorizontalSplit(incomingFilePath);
        RefreshIpcOpenSnapshot();
        return true;
    }

    return false;
}

void Ficture2Backplate::SetIpcOpenQueue(std::shared_ptr<IpcOpenQueue> queue)
{
    m_ipcQueue = std::move(queue);
    // Do not snapshot here: panes may not be named yet, and wiping would
    // clear SeedExpected from the command line.
}

void Ficture2Backplate::RefreshIpcOpenSnapshot()
{
    if (!m_ipcQueue || !m_eventBus)
    {
        return;
    }

    std::vector<std::wstring> names;
    for (auto* browser : m_eventBus->ImageBrowsersSnapshot())
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps == nullptr)
        {
            continue;
        }

        const std::wstring path = browserOps->BrowserGetDisplayedFilePath();
        std::wstring name = browserOps->BrowserGetActiveFileNameLower();
        if (name.empty())
        {
            name = IpcOpenQueue::FileNameLower(path);
        }
        if (!name.empty())
        {
            names.push_back(std::move(name));
        }
    }

    std::lock_guard<std::mutex> lock(m_ipcQueue->mutex);
    m_ipcQueue->loadedCount = names.size();
    m_ipcQueue->openNamesLower = std::move(names);
}

void Ficture2Backplate::DrainIpcOpenQueue()
{
    if (!m_ipcQueue)
    {
        return;
    }

    for (;;)
    {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(m_ipcQueue->mutex);
            if (m_ipcQueue->pending.empty())
            {
                break;
            }
            path = std::move(m_ipcQueue->pending.front());
            m_ipcQueue->pending.pop_front();
        }

        if (TryStartCompareWithFileNameMatch(path))
        {
            continue;
        }

        // Empty viewer (or no matching open name after a race): open in root.
        const auto browsers = m_eventBus ? m_eventBus->ImageBrowsersSnapshot()
                                         : std::vector<FD2D::Wnd*>{};
        bool anyOpen = false;
        for (auto* browser : browsers)
        {
            auto* ops = AsImageBrowserOps(browser);
            if (ops != nullptr && !ops->BrowserGetDisplayedFilePath().empty())
            {
                anyOpen = true;
                break;
            }
        }
        if (!anyOpen)
        {
            OpenFileInRoot(path);
            RefreshIpcOpenSnapshot();
            continue;
        }

        // Sender was told CompareStarted and exited - land the file somewhere.
        FIC2_LOG_WARN("[IPC] Drain: queued path no longer fits here - spawning a new instance.");
        wchar_t exePath[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            const std::wstring args = L"\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", exePath, args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
}

void Ficture2Backplate::OpenFileInRoot(const std::wstring& filePath)
{
    if (!m_eventBus || filePath.empty())
    {
        return;
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return;
    }

    auto* browserOps = AsImageBrowserOps(browsers.front());
    if (browserOps == nullptr)
    {
        return;
    }

    browserOps->BrowserRestoreOpenFile(filePath);
}

void Ficture2Backplate::OpenAdditionalFilesSideBySide(const std::vector<std::wstring>& filePaths)
{
    if (!m_eventBus || filePaths.empty())
    {
        return;
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return;
    }

    auto* browserOps = AsImageBrowserOps(browsers.front());
    if (browserOps == nullptr)
    {
        return;
    }

    size_t existing = browsers.size();
    for (const auto& path : filePaths)
    {
        if (path.empty())
        {
            continue;
        }

        if (existing >= ImageBrowserSplitCoordinator::kMaxViewers)
        {
            break;
        }

        browserOps->BrowserOpenAdditionalFileInHorizontalSplit(path);
        existing = m_eventBus->ImageBrowsersSnapshot().size();
    }
}

void Ficture2Backplate::OpenAdditionalFilesSideBySideAfter(const std::vector<std::wstring>& filePaths, const std::wstring& afterName)
{
    if (!m_eventBus || filePaths.empty())
    {
        return;
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return;
    }

    auto* browserOps = AsImageBrowserOps(browsers.front());
    if (browserOps == nullptr)
    {
        return;
    }

    browserOps->BrowserOpenAdditionalFilesSideBySideAfterName(filePaths, afterName);
}

bool Ficture2Backplate::ShowImageBrowserContextMenu(FD2D::Wnd* source, const POINT& ptClient)
{
    auto* browserQuery = AsImageBrowserQuery(source);
    if (browserQuery == nullptr || m_window == nullptr || m_eventBus == nullptr)
    {
        return false;
    }

    if (!browserQuery->BrowserContextMenuPrepareForDisplay(ptClient))
    {
        return false;
    }

    HMENU hMenu = LoadMenuW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_MENU_IMAGEBROWSER_CONTEXT));
    if (hMenu == nullptr)
    {
        return false;
    }

    bool shown = false;
    HMENU hPopup = GetSubMenu(hMenu, 0);
    if (hPopup != nullptr)
    {
        const auto snapshot = browserQuery->BrowserContextMenuSnapshotAtPoint(ptClient);

        bool canClose = false;
        const auto browsers = m_eventBus->ImageBrowsersSnapshot();
        if (browsers.size() >= 2)
        {
            FD2D::Wnd* rootHost = nullptr;
            for (auto* browser : browsers)
            {
                if (browser != nullptr)
                {
                    rootHost = browser;
                    break;
                }
            }
            canClose = (rootHost != nullptr && source != rootHost);
        }

        ImageBrowserContextMenu::ConfigurePayload payload {};
        payload.viewerCount = static_cast<int>(m_eventBus->ImageBrowserCount());
        payload.canClose = canClose;
        payload.showNavItems = snapshot.showNavItems;
        payload.showAlpha = !m_alphaCheckerboardEnabled;
        payload.samplingLabel = SamplingLabelForRenderer(snapshot.highQualitySampling);
        payload.hasExplorerTarget = snapshot.hasExplorerTarget;
        {
            D2D1_RECT_F mainRect {};
            payload.canSaveScreenshot =
                D3DDevice() != nullptr &&
                UseOffscreenBuffer() &&
                browserQuery->BrowserTryGetMainImageClientRect(mainRect);
        }
        payload.recentFiles = RecentFiles::Load(FICture2App::GetIniFilePath());
#if FICTURE2_ENABLE_REGISTRATION_MENU
        payload.thumbRegistered = FICture2App::IsThumbnailProviderRegistered();
        payload.associationsRegistered = FICture2App::AreFileAssociationsRegistered();
#else
        payload.thumbRegistered = false;
        payload.associationsRegistered = false;
#endif

        ImageBrowserContextMenu::Configure(hPopup, payload);
        shown = true;

        const UINT cmd = ImageBrowserContextMenu::TrackAndReturnCommand(hPopup, m_window, ptClient);
        if (cmd != 0)
        {
            if (cmd >= IDM_CTX_RECENT_BASE &&
                cmd <= IDM_CTX_RECENT_LAST &&
                (cmd - IDM_CTX_RECENT_BASE) < payload.recentFiles.size())
            {
                const std::wstring& path = payload.recentFiles[cmd - IDM_CTX_RECENT_BASE];
                auto* browserOps = AsImageBrowserOps(source);
                if (browserOps != nullptr)
                {
                    browserOps->BrowserRestoreOpenFile(path);
                }
            }
            else
            {
                (void)HandleImageBrowserContextMenuCommand(source, cmd, ptClient);
            }
        }
    }

    DestroyMenu(hMenu);
    return shown;
}

std::wstring Ficture2Backplate::SamplingLabelForRenderer(bool highQuality) const
{
    const bool usingD3D = (D3DDevice() != nullptr);
    if (usingD3D)
    {
        return highQuality ? L"D3D11 Anisotropic" : L"D3D11 Point";
    }

    const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
    if (highQuality)
    {
        return (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D HQ Cubic" : L"D2D Linear";
    }

    return (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D Nearest" : L"D2D Linear";
}

bool Ficture2Backplate::HandleImageBrowserContextMenuCommand(FD2D::Wnd* source, UINT cmd, const POINT& ptClient)
{
    auto* browserOps = AsImageBrowserOps(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    if (TryDispatchMappedAction(this, source, browserOps, cmd, kContextLifecycleMap, POINT {}, m_window))
    {
        return true;
    }

    if (TryDispatchMappedAction(this, source, browserOps, cmd, kContextViewAppearanceMap, POINT {}, m_window))
    {
        return true;
    }

    if (TryDispatchMappedAction(this, source, browserOps, cmd, kContextIntegrationMap, ptClient, m_window))
    {
        return true;
    }

#if FICTURE2_ENABLE_REGISTRATION_MENU
    if (TryDispatchMappedAction(this, source, browserOps, cmd, kContextRegistrationMap, ptClient, m_window))
    {
        return true;
    }
#endif

    return false;
}

bool Ficture2Backplate::HandleImageBrowserKeyUpCommand(
    FD2D::Wnd* source,
    UINT keyCode,
    bool ctrl,
    bool shift,
    bool alt,
    bool& outHandled)
{
    outHandled = false;

    auto* browserOps = AsImageBrowserOps(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    const UINT normalizedKey = (keyCode >= 'a' && keyCode <= 'z')
        ? (keyCode - 'a' + 'A')
        : keyCode;

    outHandled = TryDispatchMappedAction(this, source, browserOps, keyCode, kKeySelectionNavigationMap, POINT {}, m_window);
    if (outHandled)
    {
        return true;
    }

    const UINT keyForLifecycleMatch = (normalizedKey == 'O') ? normalizedKey : keyCode;
    outHandled = TryDispatchMappedKeyAction(
        this,
        source,
        browserOps,
        keyForLifecycleMatch,
        ctrl,
        shift,
        false,
        kKeyLifecycleChordMap,
        POINT {},
        m_window);
    if (outHandled)
    {
        return true;
    }

    outHandled = TryDispatchMappedKeyAction(
        this,
        source,
        browserOps,
        normalizedKey,
        false,
        shift,  // pass actual shift so Shift+<key> bindings (e.g. < and >) can match
        alt,
        kKeyViewAppearanceChordMap,
        POINT {},
        m_window);
    return outHandled;
}

bool Ficture2Backplate::HandleImageBrowserKeyDownCommand(
    FD2D::Wnd* source,
    UINT keyCode,
    bool ctrl,
    bool shift,
    bool alt,
    bool& outHandled)
{
    (void)ctrl;
    (void)shift;
    (void)alt;

    outHandled = false;

    auto* browserOps = AsImageBrowserOps(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    outHandled = TryDispatchMappedAction(this, source, browserOps, keyCode, kKeyDownSelectionNavigationMap, POINT {}, m_window);
    return outHandled;
}

std::wstring Ficture2Backplate::GetFocusedSelectedImageFileName() const
{
    if (!m_eventBus)
    {
        return L"";
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return L"";
    }

    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps != nullptr && browserOps->BrowserHasFocusForTitle())
        {
            return browserOps->BrowserSelectedImageFileNameForTitle();
        }
    }

    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps != nullptr)
        {
            return browserOps->BrowserSelectedImageFileNameForTitle();
        }
    }

    return L"";
}

void Ficture2Backplate::SaveImageBrowserSession(const std::wstring& iniFile)
{
    if (iniFile.empty())
    {
        return;
    }

    const auto browserOps = SnapshotBrowserOps(m_eventBus);
    if (browserOps.empty())
    {
        return;
    }

    auto* rootOps = browserOps.front();
    const int count = static_cast<int>((std::min)(
        ImageBrowserSplitCoordinator::kMaxViewers,
        browserOps.size()));

    ImageBrowserSessionPersistence::SavePayload payload {};
    payload.viewerCount = count;
    payload.hasThumbStripHeight = TryGetSyncedThumbStripHeight(payload.thumbStripHeight);
    payload.horizontalSplitRatios = rootOps->BrowserCaptureHorizontalSplitRatios();
    payload.viewers.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        auto* ops = browserOps[static_cast<size_t>(i)];
        if (ops == nullptr)
        {
            payload.viewers.push_back({});
            continue;
        }

        payload.viewers.push_back({
            ops->BrowserGetDisplayedFilePath(),
            ops->BrowserGetCurrentFolderPath(),
        });
    }

    ImageBrowserSessionPersistence::SaveToIni(iniFile, payload);
}

bool Ficture2Backplate::TryRestoreImageBrowserSession(const std::wstring& iniFile)
{
    if (iniFile.empty())
    {
        return false;
    }

    const auto browserOps = SnapshotBrowserOps(m_eventBus);
    if (browserOps.empty())
    {
        return false;
    }

    auto* rootOps = browserOps.front();
    if (rootOps == nullptr)
    {
        return false;
    }

    ImageBrowserSessionPersistence::RestorePayload payload {};
    if (!ImageBrowserSessionPersistence::TryRestoreFromIni(iniFile, payload))
    {
        return false;
    }

    // Keep only paths that still resolve (NIFDiff-style compact restore).
    // ViewerCount alone must not count as success — otherwise dead slots block Pictures fallback.
    std::vector<ImageBrowserSessionPersistence::RestoredViewerState> valid;
    valid.reserve(payload.viewers.size());
    for (const auto& viewer : payload.viewers)
    {
        if (!viewer.filePath.empty())
        {
            auto vp = Floar::VirtualPath::Parse(viewer.filePath);
            if (vp && vp->Exists())
            {
                valid.push_back(viewer);
                continue;
            }
            FIC2_LOG_INFO(
                "[Session] Dropping missing file from restore: {}",
                std::filesystem::path(viewer.filePath).string());
        }
        else if (!viewer.folderPath.empty())
        {
            auto vp = Floar::VirtualPath::Parse(viewer.folderPath);
            if (vp && Floar::VirtualFileSystem::IsDirectory(*vp))
            {
                valid.push_back(viewer);
                continue;
            }
            FIC2_LOG_INFO(
                "[Session] Dropping missing folder from restore: {}",
                std::filesystem::path(viewer.folderPath).string());
        }
    }

    if (valid.empty())
    {
        FIC2_LOG_INFO("[Session] Restore found no surviving viewers — treating as not restored.");
        return false;
    }

    if (valid.size() != payload.viewers.size())
    {
        FIC2_LOG_INFO(
            "[Session] Compacted restore: {} of {} viewers survived",
            valid.size(),
            payload.viewers.size());
    }

    payload.viewers = std::move(valid);
    payload.clampedViewerCount = static_cast<int>(
        (std::min)(ImageBrowserSplitCoordinator::kMaxViewers, payload.viewers.size()));
    payload.viewers.resize(static_cast<size_t>(payload.clampedViewerCount));

    if (payload.hasThumbStripHeight)
    {
        SetSyncedThumbStripHeight(payload.thumbStripHeight);
    }

    if (!payload.viewers[0].filePath.empty())
    {
        rootOps->BrowserRestoreOpenFile(payload.viewers[0].filePath);
    }
    else if (!payload.viewers[0].folderPath.empty())
    {
        rootOps->BrowserRestoreOpenFolder(payload.viewers[0].folderPath);
    }

    for (int i = 1; i < payload.clampedViewerCount; ++i)
    {
        const auto& viewer = payload.viewers[static_cast<size_t>(i)];
        if (!viewer.filePath.empty())
        {
            rootOps->BrowserAddHorizontalViewerForRestore(viewer.filePath);
        }
        else if (!viewer.folderPath.empty())
        {
            rootOps->BrowserAddHorizontalViewerForRestoreFolder(viewer.folderPath);
        }
    }

    if (payload.hasThumbStripHeight)
    {
        const auto refreshedBrowserOps = SnapshotBrowserOps(m_eventBus);
        for (auto* ops : refreshedBrowserOps)
        {
            if (ops != nullptr)
            {
                ops->BrowserForceApplySyncedThumbStripHeight();
            }
        }
    }

    // Best-effort prefix when fewer panes survived than saved ratios.
    if (!payload.horizontalSplitRatios.empty())
    {
        rootOps->BrowserApplyHorizontalSplitRatios(payload.horizontalSplitRatios);
    }

    return true;
}

void Ficture2Backplate::UpdateTitleBarInfo()
{
    if (m_window == nullptr)
        return;

    const std::wstring selectedFileName = GetFocusedSelectedImageFileName();

    // Get Direct2D version
    const char* d2dVersionStr = FD2D::Core::GetD2DVersionString();

    // Extract version number (e.g., "Direct2D 1.3 (Windows 10+)" -> "1.3")
    std::string d2dVersionA = d2dVersionStr ? d2dVersionStr : "";
    size_t versionPos = d2dVersionA.find("1.");
    std::string versionNum = "1.0";
    if (versionPos != std::string::npos)
    {
        size_t endPos = versionPos + 3; // "1.x"
        if (endPos > d2dVersionA.length())
            endPos = d2dVersionA.length();
        versionNum = d2dVersionA.substr(versionPos, endPos - versionPos);
    }

    // Convert to wide string (version string is ASCII digits + dot).
    const std::wstring versionNumW(versionNum.begin(), versionNum.end());

    // Check if using D3D11 renderer
    bool usingD3D11 = (m_rendererId.empty() || m_rendererId == L"d3d11_swapchain");

    std::wstring newTitle;
    newTitle = std::format(L"FICTure2 [Renderer: {} | D2D {}] - {}", usingD3D11 ? L"D3D11" : L"D2D", versionNumW, selectedFileName);

    SetWindowTextW(m_window, newTitle.c_str());
}
