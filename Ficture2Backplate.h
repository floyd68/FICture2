#pragma once

#include "AppDialogs.h"
#include "FD2D/Backplate.h"
#include "ImageViewTypes.h"
#include "IpcOpenRequest.h"
#include "RecentFiles.h"
#include "ViewerSettings.h"

#include <memory>
#include <string>
#include <vector>

class FolderListingWakeup;

class Ficture2Backplate : public FD2D::Backplate
{
public:
    using FD2D::Backplate::Backplate;

    class EventBus
    {
    public:
        void RegisterImageBrowser(FD2D::Wnd* browser);
        void UnregisterImageBrowser(FD2D::Wnd* browser);
        std::vector<FD2D::Wnd*> ImageBrowsersSnapshot() const
        {
			return m_imageBrowsers;
        }
        size_t ImageBrowserCount() const
		{
			return m_imageBrowsers.size();
		}

    private:
        std::vector<FD2D::Wnd*> m_imageBrowsers {};
    };

    std::shared_ptr<EventBus> BusPtr() const { return m_eventBus; }
    EventBus& Bus() { return *m_eventBus; }
    const EventBus& Bus() const { return *m_eventBus; }

    void EnsureImageBrowserIniInitialized();
    bool ShowNavItemsEnabled() const { return m_settings.ShowNavItems(); }
    D2D1_COLOR_F FocusedBackgroundColor() const { return m_settings.FocusedBackgroundColor(); }
    bool AlphaCheckerboardEnabled() const { return m_settings.AlphaCheckerboard(); }
    float ImageZoomStiffness() const { return m_settings.ZoomStiffness(); }

    void SetSyncedThumbStripHeight(float height);
    bool TryGetSyncedThumbStripHeight(float& outHeight) const;
    void SynchronizeThumbStripHeight(FD2D::Wnd* source, float height);
    void SynchronizeFileSelection(FD2D::Wnd* source, const std::wstring& fileNameLower);
    void SynchronizeViewTransform(
        FD2D::Wnd* source,
        const std::wstring& fileNameLower,
        const ImageViewTransform& viewTransform);
    void SynchronizeShowNavItems(FD2D::Wnd* source, bool showNavItems);
    void SynchronizeBackgroundColor(FD2D::Wnd* source, const D2D1_COLOR_F& color);
    void SynchronizeFocusedBackgroundColor(FD2D::Wnd* source, const D2D1_COLOR_F& color);
    void SynchronizeAlphaCheckerboard(FD2D::Wnd* source, bool checkerEnabled);
    bool TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath);
    void SetIpcOpenQueue(std::shared_ptr<IpcOpenQueue> queue);

    // The process's recent-files list, kept in iniPath and flushed by a debounce
    // timer on this window. Until this is called (tests) selections are not
    // recorded.
    void EnableRecentFiles(const std::wstring& iniPath);
    void AddRecentFile(const std::wstring& path);
    void ClearRecentFiles();
    // The last flush, from the window's before-destroy callback: stops the
    // timer, and selections after it are ignored.
    void FlushRecentFilesBeforeDestroy();

    // Wakes this window when folder-listing chunks arrive (null before the
    // window exists); disconnected when the window is destroyed.
    std::shared_ptr<FolderListingWakeup> FolderListingWakeupTarget() const { return m_folderListingWakeup; }
    // Delivers a folder-listing wakeup to every top-level Wnd; each image
    // browser drains its own lane and passes it to nested browsers.
    void DispatchFolderListingReady();
    void RefreshIpcOpenSnapshot();
    void DrainIpcOpenQueue();
    void OpenFileInRoot(const std::wstring& filePath);
    void OpenAdditionalFilesSideBySide(const std::vector<std::wstring>& filePaths);
    void OpenAdditionalFilesSideBySideAfter(const std::vector<std::wstring>& filePaths, const std::wstring& afterName);
    bool ShowImageBrowserContextMenu(FD2D::Wnd* source, const POINT& ptClient);
    bool HandleImageBrowserContextMenuCommand(FD2D::Wnd* source, UINT cmd, const POINT& ptClient);
    std::wstring SamplingLabelForRenderer(bool highQuality) const;
    bool HandleImageBrowserKeyUpCommand(
        FD2D::Wnd* source,
        UINT keyCode,
        bool ctrl,
        bool shift,
        bool alt,
        bool& outHandled);
    bool HandleImageBrowserKeyDownCommand(
        FD2D::Wnd* source,
        UINT keyCode,
        bool ctrl,
        bool shift,
        bool alt,
        bool& outHandled);
    std::wstring GetFocusedSelectedImageFileName() const;
    void SaveImageBrowserSession(const std::wstring& iniFile);
    bool TryRestoreImageBrowserSession(const std::wstring& iniFile);
    void UpdateTitleBarInfo() override;

    // Test seam: the browsers attached from now on do not load the user's
    // INI settings (the defaults stay).
    void SkipImageBrowserIniForTesting() { m_settings.MarkLoaded(); }

    // The dialogs the window's commands open (native unless replaced).
    AppDialogs& Dialogs() { return *m_dialogs; }
    // Test seam: commands use `dialogs` instead of the native dialogs.
    void SetDialogsForTesting(std::shared_ptr<AppDialogs> dialogs) { m_dialogs = std::move(dialogs); }

private:
    std::shared_ptr<EventBus> m_eventBus { std::make_shared<EventBus>() };
    float m_syncedThumbStripHeight { 0.0f };
    bool m_hasSyncedThumbStripHeight { false };
    ViewerSettings m_settings {};   // shared by every image browser of this window
    std::shared_ptr<AppDialogs> m_dialogs { AppDialogs::Native() };
    std::shared_ptr<IpcOpenQueue> m_ipcQueue {};
    std::unique_ptr<RecentFilesStore> m_recentFiles {};
    // Created with the window, disconnected when it is destroyed.
    std::shared_ptr<FolderListingWakeup> m_folderListingWakeup {};

protected:
    FD2D::Wnd* FindTargetWnd(const POINT& ptClient) override;
    bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;
    bool HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient) override;
};
