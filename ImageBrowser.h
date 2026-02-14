#pragma once

#include <memory>
#include <string>
#include <vector>

namespace FD2D
{
    class Wnd;
}

// Core viewer component: single main image per ImageBrowser.
// paneCount is kept for compatibility and is currently ignored (always 1).
std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, const std::wstring& initialFile = L"");

// IPC hook:
// Called by the first instance when a second launch sends a file path.
// Returns true if the request started compare mode (split view), false if ignored.
bool ImageBrowser_TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath);

// Session persistence (per-user INI):
// - Saves: open ImageBrowsers (1..4), each displayed file, thumbnail strip height, horizontal split ratios.
// - Restores only the viewer session; caller decides whether to use it (e.g., only when no cmdline file).
void ImageBrowser_SaveSessionToIni(const std::wstring& iniFile);
bool ImageBrowser_TryRestoreSessionFromIni(const std::wstring& iniFile);

// Used for startup fallback behavior (e.g., open first image in Pictures folder when no session exists).
void ImageBrowser_OpenFileInRoot(const std::wstring& filePath);

// Opens additional files in new side-by-side ImageBrowsers (up to 4 total).
void ImageBrowser_OpenAdditionalFilesSideBySide(const std::vector<std::wstring>& filePaths);

// Opens additional files after the specified ImageBrowser (by name).
// When afterName is empty, files are appended at the end.
void ImageBrowser_OpenAdditionalFilesSideBySideAfter(
    const std::vector<std::wstring>& filePaths,
    const std::wstring& afterName);

// Returns the currently focused ImageBrowser's selected image filename for title display.
// Empty when no image is selected.
std::wstring ImageBrowser_GetFocusedSelectedImageFileName();
