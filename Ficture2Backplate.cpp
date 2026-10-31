#include "Ficture2Backplate.h"

#include "AppLog.h"
#include "DeferredRenderScope.h"
#include "FolderEnumeration.h"
#include "CommonUtil.h"
#include "IniStore.h"
#include "IniStoreLog.h"
#include "FD2D/Core.h"
#include "AppSetup.h"
#include "BrowserCommandMap.h"
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
    // Recent-files debounce timer on the main window (FD2D's own timers use
    // 0xFD22 and 0xFD23).
    constexpr UINT_PTR kRecentFilesFlushTimerId = 0xF149;

    bool TryGetIniFilePath(std::wstring& outIniFile)
    {
        outIniFile = FICture2App::GetIniFilePath();
        return !outIniFile.empty();
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
        const std::shared_ptr<BrowserRegistry>& registry,
        FD2D::Wnd* exclude = nullptr)
    {
        std::vector<T*> out;
        if (!registry)
        {
            return out;
        }

        const auto browsers = registry->ImageBrowsersSnapshot();
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

    std::vector<IImageBrowserOps*> SnapshotBrowserOps(const std::shared_ptr<BrowserRegistry>& registry)
    {
        return SnapshotBrowsers<IImageBrowserOps>(registry);
    }

    std::vector<IImageBrowserSync*> SnapshotOtherBrowserSync(
        const std::shared_ptr<BrowserRegistry>& registry,
        FD2D::Wnd* source)
    {
        return SnapshotBrowsers<IImageBrowserSync>(registry, source);
    }

    struct BrowserOpsActionEntry
    {
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
        void (IImageBrowserOps::*invoke)() { nullptr };
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
        { BrowserCommandAction::MipPrevious, &IImageBrowserOps::BrowserCmdMipPrevious },
        { BrowserCommandAction::MipNext, &IImageBrowserOps::BrowserCmdMipNext },
        { BrowserCommandAction::ShowImageInformation, &IImageBrowserOps::BrowserCmdShowImageInformation },
    };

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
            if (!backplate->Dialogs().PickFolder(ownerWindow, selectedFolder))
            {
                return true;
            }

            browserOps->BrowserRestoreOpenFolder(selectedFolder);
            return true;
        }

        std::wstring selectedPath {};
        const std::wstring initialDir = AppDialogs::InitialDir(browserOps->BrowserGetCurrentFolderPath());
        const wchar_t* title = (action == BrowserCommandAction::OpenNewImage) ? L"Open New Image" : L"Open Image";
        if (!backplate->Dialogs().PickImageFile(ownerWindow, initialDir, title, selectedPath))
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

        auto* registry = backplate->RegistryPtr().get();
        if (registry == nullptr || registry->ImageBrowserCount() >= ImageBrowserSplitCoordinator::kMaxViewers)
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
        if (!backplate->Dialogs().PickColor(ownerWindow, current, nextColor))
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
        // The new colours (clear colour and every synchronized pane) appear
        // in one coalesced frame, rendered after this command.
        backplate->RequestRender();
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

        // Name the screenshot after the image actually on screen.
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
            FIC2_LOG_ERROR("[Screenshot] Save failed: {}", AppLog::ToUtf8(outPath));
            MessageBoxW(
                ownerWindow,
                (L"Failed to save screenshot:\n" + outPath).c_str(),
                L"FICture2",
                MB_OK | MB_ICONERROR);
        }

        return true;
    }

    bool TryExecuteClearRecentAction(Ficture2Backplate& backplate, BrowserCommandAction action)
    {
        if (action != BrowserCommandAction::ClearRecent)
        {
            return false;
        }

        backplate.ClearRecentFiles();
        return true;
    }

    bool TryExecuteSystemAction(Ficture2Backplate& backplate, BrowserCommandAction action, HWND ownerWindow)
    {
        if (action == BrowserCommandAction::ShowAbout)
        {
            backplate.Dialogs().ShowAbout(ownerWindow);
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

        if (TryExecuteClearRecentAction(*backplate, action))
        {
            return;
        }

        (void)TryExecuteSystemAction(*backplate, action, ownerWindow);
    }

}

FD2D::Wnd* Ficture2Backplate::FindTargetWnd(const POINT& ptClient)
{
    if (!m_registry)
    {
        return nullptr;
    }

    FD2D::Wnd* best = nullptr;
    float bestArea = 0.0f;

    const auto browsers = m_registry->ImageBrowsersSnapshot();
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
    // As in FD2D::Backplate::HandleFileDropPaths: a layout change since the
    // last frame (an IPC open, for example) is arranged before the drop
    // hit-tests panes. WM_DROPFILES reaches this directly.
    (void)LayoutIfPending();

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
        m_folderListingWakeup = std::make_shared<FolderListingWakeup>(hWnd);
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
    case WM_TIMER:
    {
        if (wParam != kRecentFilesFlushTimerId)
        {
            break;
        }
        KillTimer(hWnd, kRecentFilesFlushTimerId);
        if (m_recentFiles)
        {
            (void)m_recentFiles->Flush();
        }
        result = 0;
        return true;
    }
    case kFolderListingReadyMessage:
    {
        DispatchFolderListingReady();
        result = 0;
        return true;
    }
    case WM_DESTROY:
    {
        // No folder-listing wakeup is posted to this HWND after this.
        if (m_folderListingWakeup)
        {
            m_folderListingWakeup->Disconnect();
        }
        HandleFileDragLeave();
        UnregisterDropTarget();
        break;
    }
    default:
        break;
    }

    return Backplate::HandleMessage(hWnd, message, wParam, lParam, result);
}

void Ficture2Backplate::DispatchFolderListingReady()
{
    const FD2D::CommandEvent event { kFolderListingReadyMessage, 0, 0 };
    // Copy: a handler may change the tree.
    const std::vector<std::shared_ptr<FD2D::Wnd>> children = m_childrenOrdered;
    for (const auto& child : children)
    {
        if (child)
        {
            (void)child->OnCommandEvent(event);
        }
    }
}

void Ficture2Backplate::EnsureImageBrowserIniInitialized()
{
    if (m_settings.Loaded())
    {
        return;
    }

    std::wstring iniFile {};
    (void)TryGetIniFilePath(iniFile);
    FIC2_TIMER_START(t_ini);
    const std::optional<D2D1_COLOR_F> background = m_settings.Load(iniFile);
    FIC2_LOG_STEP(t_ini, "[IniInit] ViewerSettings::Load");
    if (background)
    {
        SetClearColor(*background);
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
    if (!m_registry || source == nullptr)
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
    if (m_registry->ImageBrowserCount() < 2)
    {
        return;
    }

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || source == nullptr || fileNameLower.empty())
    {
        return;
    }

    if (m_registry->ImageBrowserCount() < 2)
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

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || source == nullptr || fileNameLower.empty())
    {
        return;
    }

    if (m_registry->ImageBrowserCount() < 2)
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

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
    for (auto* target : targets)
    {
        if (target == nullptr)
        {
            continue;
        }

        const std::wstring targetNameLower = target->BrowserGetRequestedFileNameLower();
        if (!targetNameLower.empty() && targetNameLower == fileNameLower)
        {
            target->BrowserApplyViewTransformForSync(viewTransform);
        }
    }
}

void Ficture2Backplate::SynchronizeShowNavItems(FD2D::Wnd* source, bool showNavItems)
{
    if (!m_registry || source == nullptr)
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
    std::wstring iniFile {};
    (void)TryGetIniFilePath(iniFile);
    m_settings.SetShowNavItems(showNavItems, iniFile);

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || source == nullptr)
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
    (void)TryGetIniFilePath(iniFile);
    ViewerSettings::SaveBackgroundColor(color, iniFile);

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || source == nullptr)
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
    std::wstring iniFile {};
    (void)TryGetIniFilePath(iniFile);
    m_settings.SetFocusedBackgroundColor(color, iniFile);

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || source == nullptr)
    {
        return;
    }

    auto* sourceSync = AsImageBrowserSync(source);
    if (sourceSync == nullptr)
    {
        return;
    }
    (void)sourceSync;

    m_settings.SetAlphaCheckerboard(checkerEnabled);

    const auto targets = SnapshotOtherBrowserSync(m_registry, source);
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
    if (!m_registry || incomingFilePath.empty())
    {
        return false;
    }

    const std::wstring incomingName = IpcOpenQueue::FileNameLower(incomingFilePath);
    if (incomingName.empty())
    {
        return false;
    }

    const auto browsers = m_registry->ImageBrowsersSnapshot();
    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps == nullptr)
        {
            continue;
        }

        if (browserOps->BrowserGetRequestedFileNameLower() != incomingName)
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

void Ficture2Backplate::EnableRecentFiles(const std::wstring& iniPath)
{
    m_recentFiles = std::make_unique<RecentFilesStore>(iniPath);
    // The debounce runs on this window's UI-thread timer (no thread): SetTimer
    // with the same id re-arms it, and WM_TIMER flushes.
    m_recentFiles->SetFlushScheduler([this](bool arm)
    {
        if (m_window == nullptr)
        {
            return;
        }
        if (arm)
        {
            (void)SetTimer(m_window, kRecentFilesFlushTimerId, RecentFiles::kFlushDelayMs, nullptr);
        }
        else
        {
            KillTimer(m_window, kRecentFilesFlushTimerId);
        }
    });
}

void Ficture2Backplate::AddRecentFile(const std::wstring& path)
{
    if (m_recentFiles)
    {
        m_recentFiles->Add(path);
    }
}

void Ficture2Backplate::ClearRecentFiles()
{
    if (m_recentFiles)
    {
        (void)m_recentFiles->Clear();
    }
}

void Ficture2Backplate::FlushRecentFilesBeforeDestroy()
{
    if (m_recentFiles)
    {
        (void)m_recentFiles->FlushBeforeDestroy();
    }
}

void Ficture2Backplate::RefreshIpcOpenSnapshot()
{
    if (!m_ipcQueue || !m_registry)
    {
        return;
    }

    // Requested paths: a pane that just switched images must match the new
    // name immediately, not the previous image that is still on screen.
    std::vector<std::wstring> requested;
    for (auto* browser : m_registry->ImageBrowsersSnapshot())
    {
        if (auto* browserOps = AsImageBrowserOps(browser))
        {
            requested.push_back(browserOps->BrowserGetRequestedFilePath());
        }
    }
    m_ipcQueue->PublishOpenNames(requested);
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
        const auto browsers = m_registry ? m_registry->ImageBrowsersSnapshot()
                                         : std::vector<FD2D::Wnd*>{};
        bool anyOpen = false;
        for (auto* browser : browsers)
        {
            auto* ops = AsImageBrowserOps(browser);
            if (ops != nullptr && !ops->BrowserGetRequestedFilePath().empty())
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
    if (!m_registry || filePath.empty())
    {
        return;
    }

    const auto browsers = m_registry->ImageBrowsersSnapshot();
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
    if (!m_registry || filePaths.empty())
    {
        return;
    }

    const auto browsers = m_registry->ImageBrowsersSnapshot();
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
        existing = m_registry->ImageBrowsersSnapshot().size();
    }
}

void Ficture2Backplate::OpenAdditionalFilesSideBySideAfter(const std::vector<std::wstring>& filePaths, const std::wstring& afterName)
{
    if (!m_registry || filePaths.empty())
    {
        return;
    }

    const auto browsers = m_registry->ImageBrowsersSnapshot();
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
    if (browserQuery == nullptr || m_window == nullptr || m_registry == nullptr)
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
        const auto browsers = m_registry->ImageBrowsersSnapshot();
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
        payload.viewerCount = static_cast<int>(m_registry->ImageBrowserCount());
        payload.canClose = canClose;
        payload.showNavItems = snapshot.showNavItems;
        payload.showAlpha = !m_settings.AlphaCheckerboard();
        payload.samplingLabel = SamplingLabelForRenderer(snapshot.highQualitySampling);
        payload.hasExplorerTarget = snapshot.hasExplorerTarget;
        payload.hasImage = snapshot.hasImage;
        payload.alphaUsageOverride = snapshot.alphaUsageOverride;
        payload.mipLevels = snapshot.mipLevels;
        payload.mipLevel = snapshot.mipLevel;
        payload.sourceWidth = snapshot.sourceWidth;
        payload.sourceHeight = snapshot.sourceHeight;
        {
            D2D1_RECT_F mainRect {};
            payload.canSaveScreenshot =
                D3DDevice() != nullptr &&
                UseOffscreenBuffer() &&
                browserQuery->BrowserTryGetMainImageClientRect(mainRect);
        }
        payload.recentFiles = m_recentFiles ? m_recentFiles->MenuEntries() : std::vector<std::wstring> {};
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
            auto* browserOps = AsImageBrowserOps(source);
            if (cmd >= IDM_CTX_RECENT_BASE &&
                cmd <= IDM_CTX_RECENT_LAST &&
                (cmd - IDM_CTX_RECENT_BASE) < payload.recentFiles.size())
            {
                const std::wstring& path = payload.recentFiles[cmd - IDM_CTX_RECENT_BASE];
                if (browserOps != nullptr)
                {
                    browserOps->BrowserRestoreOpenFile(path);
                }
            }
            else if (browserOps != nullptr &&
                cmd >= IDM_CTX_MIP_BASE &&
                cmd <= IDM_CTX_MIP_LAST &&
                (cmd - IDM_CTX_MIP_BASE) < payload.mipLevels)
            {
                browserOps->BrowserCmdSelectMip(cmd - IDM_CTX_MIP_BASE);
            }
            else if (browserOps != nullptr && cmd == IDM_CTX_ALPHA_AUTO)
            {
                browserOps->BrowserCmdSetAlphaUsageOverride(ImageCore::AlphaUsage::Auto);
            }
            else if (browserOps != nullptr && cmd == IDM_CTX_ALPHA_COVERAGE)
            {
                browserOps->BrowserCmdSetAlphaUsageOverride(ImageCore::AlphaUsage::Coverage);
            }
            else if (browserOps != nullptr && cmd == IDM_CTX_ALPHA_DATA)
            {
                browserOps->BrowserCmdSetAlphaUsageOverride(ImageCore::AlphaUsage::Data);
            }
            else if (browserOps != nullptr && cmd == IDM_CTX_IMAGE_INFORMATION)
            {
                browserOps->BrowserCmdShowImageInformation();
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

    // Only Show in Explorer uses the point; the other commands ignore it.
    const auto action = BrowserCommandMap::ForContextMenu(cmd);
    if (!action)
    {
        return false;
    }
    ExecuteBrowserCommandAction(this, source, browserOps, *action, ptClient, m_window);
    return true;
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

    const auto action = BrowserCommandMap::ForKeyUp(keyCode, ctrl, shift, alt);
    if (!action)
    {
        return false;
    }
    ExecuteBrowserCommandAction(this, source, browserOps, *action, POINT {}, m_window);
    outHandled = true;
    return true;
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

    const auto action = BrowserCommandMap::ForKeyDown(keyCode);
    if (!action)
    {
        return false;
    }
    ExecuteBrowserCommandAction(this, source, browserOps, *action, POINT {}, m_window);
    outHandled = true;
    return true;
}

std::wstring Ficture2Backplate::GetFocusedSelectedImageFileName() const
{
    if (!m_registry)
    {
        return L"";
    }

    const auto browsers = m_registry->ImageBrowsersSnapshot();
    if (browsers.empty())
    {
        return L"";
    }

    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps != nullptr && browserOps->BrowserHasFocusForTitle())
        {
            return browserOps->BrowserRequestedImageFileNameForTitle();
        }
    }

    for (auto* browser : browsers)
    {
        auto* browserOps = AsImageBrowserOps(browser);
        if (browserOps != nullptr)
        {
            return browserOps->BrowserRequestedImageFileNameForTitle();
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

    const auto browserOps = SnapshotBrowserOps(m_registry);
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

        // The requested image: what the user selected, even if it has not
        // finished loading (restore drops it if it no longer exists).
        payload.viewers.push_back({
            ops->BrowserGetRequestedFilePath(),
            ops->BrowserGetCurrentFolderPath(),
        });
    }

    LogIniWrite(ImageBrowserSessionPersistence::SaveToIni(iniFile, payload), "Session");
}

bool Ficture2Backplate::TryRestoreImageBrowserSession(const std::wstring& iniFile)
{
    if (iniFile.empty())
    {
        return false;
    }

    const auto browserOps = SnapshotBrowserOps(m_registry);
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
            if (vp && Floar::VirtualFileSystem::Exists(*vp))
            {
                valid.push_back(viewer);
                continue;
            }
            FIC2_LOG_INFO(
                "[Session] Dropping missing file from restore: {}",
                AppLog::ToUtf8(viewer.filePath));
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
                AppLog::ToUtf8(viewer.folderPath));
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

    // Restoring the viewers (and their settings) renders one frame in all.
    const DeferredRenderScope batchRender(this);

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
        // The added viewers' layout requests are laid out by the next
        // (coalesced) render, but the forced height is computed from each
        // browser's arranged root split: arrange the rebuilt host now (no
        // frame).
        (void)LayoutIfPending();
        const auto refreshedBrowserOps = SnapshotBrowserOps(m_registry);
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
