#pragma once

#include <windows.h>

#include <optional>

// The user-facing commands of an image browser.
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
    MipPrevious,
    MipNext,
    ShowImageInformation,
    ShowInExplorerAtPoint,
    ShowAbout,
    SaveScreenshot,
    ClearRecent,
    RegisterAssociations,
    UnregisterAssociations,
    RegisterThumbnailProvider,
    UnregisterThumbnailProvider
};

// Which command a context-menu item or a key runs: the single source of truth
// for user-facing command routing. The window (Ficture2Backplate) executes
// the command.
namespace BrowserCommandMap
{
    // The command of a context-menu item (registration items only when the
    // registration menu is built in).
    std::optional<BrowserCommandAction> ForContextMenu(UINT id);

    // The command a key-up runs, first match wins:
    //  - Enter, Home, End, PageUp, PageDown, Backspace, whatever the modifiers;
    //  - Ctrl+F4 / Ctrl+Shift+F4 and Ctrl+O / Ctrl+Shift+O (O in either case;
    //    Alt is ignored);
    //  - Ctrl+[ / Ctrl+], with exactly these modifiers;
    //  - Alt+Up and Alt+N/A/X/B/Q (letters in either case), Shift+, and
    //    Shift+. - these ignore Ctrl.
    std::optional<BrowserCommandAction> ForKeyUp(UINT key, bool ctrl, bool shift, bool alt);

    // The command a key-down runs: Left / Right, whatever the modifiers.
    std::optional<BrowserCommandAction> ForKeyDown(UINT key);
}
