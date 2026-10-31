#pragma once

#include <d2d1.h>
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// The modal dialogs the window's commands open. Native() shows the Win32
// dialogs; tests substitute their own (Ficture2Backplate::SetDialogsForTesting)
// so the commands run without blocking on a dialog.
class AppDialogs
{
public:
    virtual ~AppDialogs() = default;

    // An existing image file of a supported type. False when cancelled (or
    // the pick is not a supported image, which the native dialog reports).
    virtual bool PickImageFile(HWND owner, const std::wstring& initialDir, const wchar_t* title, std::wstring& outPath) = 0;
    // An existing folder. False when cancelled.
    virtual bool PickFolder(HWND owner, std::wstring& outFolder) = 0;
    // A colour, starting from `current`. False when cancelled.
    virtual bool PickColor(HWND owner, const D2D1_COLOR_F& current, D2D1_COLOR_F& outColor) = 0;
    virtual void ShowAbout(HWND owner) = 0;

    static std::shared_ptr<AppDialogs> Native();

    // The About dialog: builds its content and hands it to `show`
    // (TaskDialogIndirect natively) and returns what `show` returns. The icon
    // in the content is loaded for this call and released when `show`
    // returns, whatever it returns.
    using ShowTaskDialog = std::function<HRESULT(const TASKDIALOGCONFIG&)>;
    static HRESULT ShowAboutWith(HWND owner, const ShowTaskDialog& show);

    // The open-file filter: "Supported images (*.a;*.b)" then "All files
    // (*.*)", each as a description/pattern pair, double-null terminated.
    // Without extensions the first pattern is *.* too.
    static std::wstring SupportedImageFilter(const std::vector<std::wstring>& extensions);
    // The folder a file dialog starts in: `folder` when it exists, else none.
    static std::wstring InitialDir(const std::wstring& folder);
};
