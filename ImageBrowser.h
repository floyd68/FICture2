#pragma once

#include "FD2D/Image.h"

#include <memory>
#include <string>
#include <vector>

namespace FD2D
{
    class Wnd;
}

class IImageBrowserSync
{
public:
    virtual ~IImageBrowserSync() = default;

    virtual void BrowserForceApplySyncedThumbStripHeight() = 0;
    virtual void BrowserSelectFileNameForSync(const std::wstring& fileNameLower) = 0;
    virtual std::wstring BrowserGetActiveFileNameLower() const = 0;
    virtual void BrowserApplyViewTransformForSync(const FD2D::Image::ViewTransform& vt) = 0;
    virtual void BrowserApplyShowNavItemsForSync(bool showNavItems) = 0;
    virtual void BrowserApplyBackgroundColorForSync(const D2D1_COLOR_F& color) = 0;
    virtual void BrowserApplyFocusedBackgroundColorForSync(const D2D1_COLOR_F& color) = 0;
    virtual void BrowserApplyAlphaCheckerboardForSync(bool checkerEnabled) = 0;
};

class IImageBrowserCommands
{
public:
    virtual ~IImageBrowserCommands() = default;

    // Command domain: file/viewer lifecycle.
    virtual void BrowserCmdOpenImage() = 0;
    virtual void BrowserCmdOpenImageSplitNew() = 0;
    virtual void BrowserCmdOpenNewImage() = 0;
    virtual void BrowserCmdClose() = 0;

    // Command domain: selection/navigation.
    virtual void BrowserCmdActivateSelected() = 0;
    virtual void BrowserCmdSelectPrevious() = 0;
    virtual void BrowserCmdSelectNext() = 0;
    virtual void BrowserCmdSelectFirst() = 0;
    virtual void BrowserCmdSelectLast() = 0;
    virtual void BrowserCmdPagePrevious() = 0;
    virtual void BrowserCmdPageNext() = 0;
    virtual void BrowserCmdNavigateUp() = 0;

    // Command domain: view/appearance toggles.
    virtual void BrowserCmdBackgroundColor() = 0;
    virtual void BrowserCmdFocusedBackgroundColor() = 0;
    virtual void BrowserCmdFitToScreen() = 0;
    virtual void BrowserCmdToggleDirectories() = 0;
    virtual void BrowserCmdToggleAlpha() = 0;
    virtual void BrowserCmdToggleSampling() = 0;

    // Command domain: integration/system actions.
    virtual void BrowserCmdShowInExplorerAtPoint(const POINT& ptClient) = 0;
    virtual void BrowserCmdRegisterAssociations() = 0;
    virtual void BrowserCmdRegisterThumbnailProvider() = 0;
    virtual void BrowserCmdUnregisterThumbnailProvider() = 0;
};

class IImageBrowserQuery
{
public:
    struct ContextMenuSnapshot
    {
        bool showNavItems { true };
        bool highQualitySampling { true };
        bool hasExplorerTarget { false };
    };

    virtual ~IImageBrowserQuery() = default;

    virtual std::wstring BrowserGetDisplayedFilePath() const = 0;
    virtual std::wstring BrowserGetCurrentFolderPath() const = 0;
    virtual std::vector<float> BrowserCaptureHorizontalSplitRatios() const = 0;
    virtual bool BrowserContextMenuPrepareForDisplay(const POINT& ptClient) = 0;
    virtual ContextMenuSnapshot BrowserContextMenuSnapshotAtPoint(const POINT& ptClient) const = 0;
    virtual bool BrowserHasFocusForTitle() const = 0;
    virtual std::wstring BrowserSelectedImageFileNameForTitle() const = 0;
};

class IImageBrowserOps : public IImageBrowserSync, public IImageBrowserCommands, public IImageBrowserQuery
{
public:
    using ContextMenuSnapshot = IImageBrowserQuery::ContextMenuSnapshot;

    virtual ~IImageBrowserOps() = default;

    virtual bool BrowserTryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath) = 0;
    virtual void BrowserRestoreOpenFile(const std::wstring& filePath) = 0;
    virtual void BrowserOpenAdditionalFileInHorizontalSplit(const std::wstring& filePath) = 0;
    virtual void BrowserOpenAdditionalFilesSideBySideAfterName(
        const std::vector<std::wstring>& filePaths,
        const std::wstring& afterName) = 0;
    virtual void BrowserRestoreOpenFolder(const std::wstring& folderPath) = 0;
    virtual void BrowserAddHorizontalViewerForRestore(const std::wstring& filePath) = 0;
    virtual void BrowserAddHorizontalViewerForRestoreFolder(const std::wstring& folderPath) = 0;
    virtual void BrowserForceApplySyncedThumbStripHeight() = 0;
    virtual void BrowserApplyHorizontalSplitRatios(const std::vector<float>& ratios) = 0;
};

// Core viewer component: single main image per ImageBrowser.
// paneCount is kept for compatibility and is currently ignored (always 1).
std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, const std::wstring& initialFile = L"");
