#include "Ficture2Backplate.h"

#include "FD2D/Core.h"
#include "AppSetup.h"
#include "Resource.h"
#include "Version.h"
#include "ImageBrowser.h"
#include "ImageBrowserContextMenu.h"
#include "ImageBrowserSessionPersistence.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <commctrl.h>
#include <shellapi.h>

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

        const auto toByte = [](float v) -> unsigned
        {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            return static_cast<unsigned>(std::floor(v * 255.0f + 0.5f));
        };

        wchar_t rgb[64] {};
        swprintf_s(rgb, L"%u,%u,%u", toByte(color.r), toByte(color.g), toByte(color.b));
        (void)WritePrivateProfileStringW(section, key, rgb, iniFile.c_str());
    }

    IImageBrowserOps* AsImageBrowserOps(FD2D::Wnd* wnd)
    {
        if (wnd == nullptr)
        {
            return nullptr;
        }

        return dynamic_cast<IImageBrowserOps*>(wnd);
    }

    IImageBrowserCommands* AsImageBrowserCommands(FD2D::Wnd* wnd)
    {
        if (wnd == nullptr)
        {
            return nullptr;
        }

        return dynamic_cast<IImageBrowserCommands*>(wnd);
    }

    IImageBrowserSync* AsImageBrowserSync(FD2D::Wnd* wnd)
    {
        if (wnd == nullptr)
        {
            return nullptr;
        }

        return dynamic_cast<IImageBrowserSync*>(wnd);
    }

    IImageBrowserQuery* AsImageBrowserQuery(FD2D::Wnd* wnd)
    {
        if (wnd == nullptr)
        {
            return nullptr;
        }

        return dynamic_cast<IImageBrowserQuery*>(wnd);
    }

    std::vector<IImageBrowserOps*> SnapshotBrowserOps(const std::shared_ptr<Ficture2Backplate::EventBus>& bus)
    {
        std::vector<IImageBrowserOps*> out;
        if (!bus)
        {
            return out;
        }

        const auto browsers = bus->ImageBrowsersSnapshot();
        out.reserve(browsers.size());
        for (auto* browser : browsers)
        {
            auto* ops = AsImageBrowserOps(browser);
            if (ops != nullptr)
            {
                out.push_back(ops);
            }
        }

        return out;
    }

    std::vector<IImageBrowserSync*> SnapshotOtherBrowserSync(
        const std::shared_ptr<Ficture2Backplate::EventBus>& bus,
        FD2D::Wnd* source)
    {
        std::vector<IImageBrowserSync*> out;
        if (!bus)
        {
            return out;
        }

        const auto browsers = bus->ImageBrowsersSnapshot();
        out.reserve(browsers.size());
        for (auto* browser : browsers)
        {
            if (browser == nullptr || browser == source)
            {
                continue;
            }

            auto* sync = AsImageBrowserSync(browser);
            if (sync != nullptr)
            {
                out.push_back(sync);
            }
        }

        return out;
    }

    std::vector<IImageBrowserOps*> SnapshotOtherBrowserOps(
        const std::shared_ptr<Ficture2Backplate::EventBus>& bus,
        FD2D::Wnd* source)
    {
        std::vector<IImageBrowserOps*> out;
        if (!bus)
        {
            return out;
        }

        const auto browsers = bus->ImageBrowsersSnapshot();
        out.reserve(browsers.size());
        for (auto* browser : browsers)
        {
            if (browser == nullptr || browser == source)
            {
                continue;
            }

            auto* ops = AsImageBrowserOps(browser);
            if (ops != nullptr)
            {
                out.push_back(ops);
            }
        }

        return out;
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
        ShowInExplorerAtPoint,
        ShowAbout,
        RegisterAssociations,
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

    // Centralized command/key policy maps.
    // Keep this section as the single source of truth for user-facing command routing.

    // Context menu: file/viewer lifecycle commands.
    static const CommandMapEntry kContextLifecycleMap[] =
    {
        { IDM_CTX_OPEN_IMAGE, BrowserCommandAction::OpenImage },
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
    };

    // Context menu: integration/system commands.
    static const CommandMapEntry kContextIntegrationMap[] =
    {
        { IDM_CTX_SHOW_IN_EXPLORER, BrowserCommandAction::ShowInExplorerAtPoint },
        { IDM_CTX_ABOUT, BrowserCommandAction::ShowAbout },
    };

#if FICTURE2_ENABLE_REGISTRATION_MENU
    static const CommandMapEntry kContextRegistrationMap[] =
    {
        { IDM_CTX_REGISTER_ASSOCIATIONS, BrowserCommandAction::RegisterAssociations },
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
    };

    void ExecuteBrowserCommandAction(
        IImageBrowserCommands* browserOps,
        BrowserCommandAction action,
        const POINT& ptClient,
        HWND ownerWindow);

    template <size_t N>
    bool TryDispatchMappedAction(
        IImageBrowserCommands* browserOps,
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
                ExecuteBrowserCommandAction(browserOps, entry.action, ptClient, ownerWindow);
                return true;
            }
        }

        return false;
    }

    template <size_t N>
    bool TryDispatchMappedKeyAction(
        IImageBrowserCommands* browserOps,
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
                ExecuteBrowserCommandAction(browserOps, entry.action, ptClient, ownerWindow);
                return true;
            }
        }

        return false;
    }

    void ExecuteBrowserCommandAction(
        IImageBrowserCommands* browserOps,
        BrowserCommandAction action,
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (browserOps == nullptr)
        {
            return;
        }

        switch (action)
        {
        case BrowserCommandAction::OpenImage:
            browserOps->BrowserCmdOpenImage();
            return;
        case BrowserCommandAction::OpenImageSplitNew:
            browserOps->BrowserCmdOpenImageSplitNew();
            return;
        case BrowserCommandAction::OpenNewImage:
            browserOps->BrowserCmdOpenNewImage();
            return;
        case BrowserCommandAction::Close:
            browserOps->BrowserCmdClose();
            return;
        case BrowserCommandAction::ActivateSelected:
            browserOps->BrowserCmdActivateSelected();
            return;
        case BrowserCommandAction::SelectPrevious:
            browserOps->BrowserCmdSelectPrevious();
            return;
        case BrowserCommandAction::SelectNext:
            browserOps->BrowserCmdSelectNext();
            return;
        case BrowserCommandAction::SelectFirst:
            browserOps->BrowserCmdSelectFirst();
            return;
        case BrowserCommandAction::SelectLast:
            browserOps->BrowserCmdSelectLast();
            return;
        case BrowserCommandAction::PagePrevious:
            browserOps->BrowserCmdPagePrevious();
            return;
        case BrowserCommandAction::PageNext:
            browserOps->BrowserCmdPageNext();
            return;
        case BrowserCommandAction::NavigateUp:
            browserOps->BrowserCmdNavigateUp();
            return;
        case BrowserCommandAction::FitToScreen:
            browserOps->BrowserCmdFitToScreen();
            return;
        case BrowserCommandAction::BackgroundColor:
            browserOps->BrowserCmdBackgroundColor();
            return;
        case BrowserCommandAction::FocusedBackgroundColor:
            browserOps->BrowserCmdFocusedBackgroundColor();
            return;
        case BrowserCommandAction::ToggleDirectories:
            browserOps->BrowserCmdToggleDirectories();
            return;
        case BrowserCommandAction::ToggleAlpha:
            browserOps->BrowserCmdToggleAlpha();
            return;
        case BrowserCommandAction::ToggleSampling:
            browserOps->BrowserCmdToggleSampling();
            return;
        case BrowserCommandAction::ShowInExplorerAtPoint:
            browserOps->BrowserCmdShowInExplorerAtPoint(ptClient);
            return;
        case BrowserCommandAction::ShowAbout:
            ShowAboutDialog(ownerWindow);
            return;
        case BrowserCommandAction::RegisterAssociations:
            browserOps->BrowserCmdRegisterAssociations();
            return;
        case BrowserCommandAction::RegisterThumbnailProvider:
            browserOps->BrowserCmdRegisterThumbnailProvider();
            return;
        case BrowserCommandAction::UnregisterThumbnailProvider:
            browserOps->BrowserCmdUnregisterThumbnailProvider();
            return;
        }
    }

    bool TryHandleContextCommandLifecycle(IImageBrowserCommands* browserOps, UINT cmd)
    {
        return TryDispatchMappedAction(browserOps, cmd, kContextLifecycleMap, POINT {}, nullptr);
    }

    bool TryHandleContextCommandViewAppearance(IImageBrowserCommands* browserOps, UINT cmd)
    {
        return TryDispatchMappedAction(browserOps, cmd, kContextViewAppearanceMap, POINT {}, nullptr);
    }

    bool TryHandleContextCommandIntegration(
        IImageBrowserCommands* browserOps,
        UINT cmd,
        const POINT& ptClient,
        HWND ownerWindow)
    {
        if (TryDispatchMappedAction(browserOps, cmd, kContextIntegrationMap, ptClient, ownerWindow))
        {
            return true;
        }

#if FICTURE2_ENABLE_REGISTRATION_MENU
        if (TryDispatchMappedAction(browserOps, cmd, kContextRegistrationMap, ptClient, ownerWindow))
        {
            return true;
        }
#endif
        return false;
    }

    bool TryHandleKeyCommandLifecycle(
        IImageBrowserCommands* browserOps,
        UINT keyCode,
        UINT normalizedKey,
        bool ctrl,
        bool shift,
        bool& outHandled)
    {
        const UINT keyForMatch = (normalizedKey == 'O') ? normalizedKey : keyCode;
        outHandled = TryDispatchMappedKeyAction(
            browserOps,
            keyForMatch,
            ctrl,
            shift,
            false,
            kKeyLifecycleChordMap,
            POINT {},
            nullptr);
        return outHandled;
    }

    bool TryHandleKeyCommandSelectionNavigation(
        IImageBrowserCommands* browserOps,
        UINT keyCode,
        bool& outHandled)
    {
        outHandled = TryDispatchMappedAction(browserOps, keyCode, kKeySelectionNavigationMap, POINT {}, nullptr);
        return outHandled;
    }

    bool TryHandleKeyCommandViewAppearance(
        IImageBrowserCommands* browserOps,
        UINT normalizedKey,
        bool alt,
        bool& outHandled)
    {
        outHandled = TryDispatchMappedKeyAction(
            browserOps,
            normalizedKey,
            false,
            false,
            alt,
            kKeyViewAppearanceChordMap,
            POINT {},
            nullptr);
        return outHandled;
    }

    bool TryHandleKeyDownSelectionNavigation(
        IImageBrowserCommands* browserOps,
        UINT keyCode,
        bool& outHandled)
    {
        outHandled = TryDispatchMappedAction(browserOps, keyCode, kKeyDownSelectionNavigationMap, POINT {}, nullptr);
        return outHandled;
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

FD2D::Wnd* Ficture2Backplate::FindImageBrowserTarget(const POINT& ptClient) const
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

FD2D::Wnd* Ficture2Backplate::FindTargetWnd(const POINT& ptClient)
{
    return FindImageBrowserTarget(ptClient);
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
        auto vp = VirtualPath::Parse(path);
        if (!vp)
        {
            continue;
        }

        if (VirtualFileSystem::IsDirectory(*vp) || vp->IsArchiveFile() || VirtualFileSystem::IsImageFile(*vp))
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
            insertMode = (relX >= 0.75f);
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

    const int showNav = GetPrivateProfileIntW(L"Viewer", L"ShowNavItems", 1, iniFile.c_str());
    m_showNavItems = (showNav != 0);

    wchar_t buf[128] {};
    if (GetPrivateProfileStringW(L"Window", L"BackgroundColor", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str()) > 0)
    {
        D2D1_COLOR_F color {};
        if (TryParseRgb(buf, color))
        {
            SetClearColor(color);
        }
    }

    wchar_t focusedBuf[128] {};
    if (GetPrivateProfileStringW(L"Window", L"FocusedBackgroundColor", L"", focusedBuf, static_cast<DWORD>(std::size(focusedBuf)), iniFile.c_str()) > 0)
    {
        D2D1_COLOR_F focusedColor {};
        if (TryParseRgb(focusedBuf, focusedColor))
        {
            m_focusedBackgroundColor = focusedColor;
        }
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
    const FD2D::Image::ViewTransform& viewTransform)
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
        (void)WritePrivateProfileStringW(L"Viewer", L"ShowNavItems", m_showNavItems ? L"1" : L"0", iniFile.c_str());
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
    if (!m_eventBus)
    {
        return false;
    }

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return false;
    }

    auto* browserOps = AsImageBrowserOps(browsers.front());
    if (browserOps == nullptr)
    {
        return false;
    }

    return browserOps->BrowserTryStartCompareWithFileNameMatch(incomingFilePath);
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

        if (existing >= 4)
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
#if FICTURE2_ENABLE_REGISTRATION_MENU
        payload.thumbRegistered = FICture2App::IsThumbnailProviderRegistered();
#else
        payload.thumbRegistered = false;
#endif

        ImageBrowserContextMenu::Configure(hPopup, payload);
        shown = true;

        const UINT cmd = ImageBrowserContextMenu::TrackAndReturnCommand(hPopup, m_window, ptClient);
        if (cmd != 0)
        {
            (void)HandleImageBrowserContextMenuCommand(source, cmd, ptClient);
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
    auto* browserOps = AsImageBrowserCommands(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    if (TryHandleContextCommandLifecycle(browserOps, cmd))
    {
        return true;
    }

    if (TryHandleContextCommandViewAppearance(browserOps, cmd))
    {
        return true;
    }

    return TryHandleContextCommandIntegration(browserOps, cmd, ptClient, m_window);
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

    auto* browserOps = AsImageBrowserCommands(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    const UINT normalizedKey = (keyCode >= 'a' && keyCode <= 'z')
        ? (keyCode - 'a' + 'A')
        : keyCode;

    if (TryHandleKeyCommandSelectionNavigation(browserOps, keyCode, outHandled))
    {
        return true;
    }

    if (TryHandleKeyCommandLifecycle(browserOps, keyCode, normalizedKey, ctrl, shift, outHandled))
    {
        return true;
    }

    return TryHandleKeyCommandViewAppearance(browserOps, normalizedKey, alt, outHandled);
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

    auto* browserOps = AsImageBrowserCommands(source);
    if (browserOps == nullptr)
    {
        return false;
    }

    return TryHandleKeyDownSelectionNavigation(browserOps, keyCode, outHandled);
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
    const int count = static_cast<int>((std::min)(static_cast<size_t>(4), browserOps.size()));

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

    if (payload.hasThumbStripHeight)
    {
        SetSyncedThumbStripHeight(payload.thumbStripHeight);
    }

    if (!payload.viewers.empty())
    {
        if (!payload.viewers[0].filePath.empty())
        {
            rootOps->BrowserRestoreOpenFile(payload.viewers[0].filePath);
        }
        else if (!payload.viewers[0].folderPath.empty())
        {
            rootOps->BrowserRestoreOpenFolder(payload.viewers[0].folderPath);
        }
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
