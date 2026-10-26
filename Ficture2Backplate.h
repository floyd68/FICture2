#pragma once

#include "DeferredRenderScope.h"
#include "FD2D/Backplate.h"
#include "ImageViewTypes.h"
#include "IpcOpenRequest.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FolderListingWakeup;

class Ficture2Backplate : public FD2D::Backplate, public DeferredLayoutHost
{
public:
    using FD2D::Backplate::Backplate;

    struct ImageBrowserEvent
    {
        enum class Type
        {
            ThumbStripHeightChanged,
            FileNameSelected,
            ViewTransformChanged,
            ShowNavItemsChanged,
            BackgroundColorChanged,
            FocusedBackgroundColorChanged,
            AlphaCheckerboardChanged
        };

        Type type { Type::ThumbStripHeightChanged };
        FD2D::Wnd* source { nullptr };
        float thumbStripHeight { 0.0f };
        std::wstring fileNameLower {};
        ImageViewTransform viewTransform {};
        bool boolValue { false };
        D2D1_COLOR_F color { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    class EventBus
    {
    public:
        using HandlerId = std::uint64_t;
        using Handler = std::function<void(const ImageBrowserEvent&)>;

        HandlerId Subscribe(Handler handler);
        void Unsubscribe(HandlerId id);
        void Publish(const ImageBrowserEvent& event);

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
        HandlerId m_nextId { 1 };
        std::unordered_map<HandlerId, Handler> m_handlers {};
        std::vector<FD2D::Wnd*> m_imageBrowsers {};
    };

    std::shared_ptr<EventBus> BusPtr() const { return m_eventBus; }
    EventBus& Bus() { return *m_eventBus; }
    const EventBus& Bus() const { return *m_eventBus; }

    void EnsureImageBrowserIniInitialized();
    bool ShowNavItemsEnabled() const { return m_showNavItems; }
    D2D1_COLOR_F FocusedBackgroundColor() const { return m_focusedBackgroundColor; }
    bool AlphaCheckerboardEnabled() const { return m_alphaCheckerboardEnabled; }
    float ImageZoomStiffness() const { return m_imageZoomStiffness; }

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
    void SkipImageBrowserIniForTesting() { m_imageBrowserIniInitialized = true; }

    // Measures and arranges the top-level Wnds in the client area, as the
    // next render's layout pass will, without rendering: for code that needs
    // arranged bounds while a DeferredRenderScope holds the layout request.
    void ArrangeChildrenNow() override;

private:
    std::shared_ptr<EventBus> m_eventBus { std::make_shared<EventBus>() };
    float m_syncedThumbStripHeight { 0.0f };
    bool m_hasSyncedThumbStripHeight { false };
    bool m_showNavItems { true };
    bool m_alphaCheckerboardEnabled { false };
    D2D1_COLOR_F m_focusedBackgroundColor { 0.18f, 0.16f, 0.03f, 1.0f };
    float m_imageZoomStiffness { 80.0f };
    bool m_imageBrowserIniInitialized { false };
    std::shared_ptr<IpcOpenQueue> m_ipcQueue {};
    // Created with the window, disconnected when it is destroyed.
    std::shared_ptr<FolderListingWakeup> m_folderListingWakeup {};

protected:
    FD2D::Wnd* FindTargetWnd(const POINT& ptClient) override;
    bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;
    bool HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient) override;
};
