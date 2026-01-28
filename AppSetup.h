#pragma once

#include <string>

// Forward declare Win32 HWND without pulling in <windows.h> from this header.
struct HWND__;
using HWND = HWND__*;

namespace FICture2App
{
    // %LOCALAPPDATA%\FICture2\FICture2.ini
    std::wstring GetIniFilePath();

    // First-run is defined as "INI does not exist".
    // If first-run, prompts the user and (optionally) registers per-user (HKCU) file associations.
    // Always creates the INI (best-effort) so the prompt is shown only once.
    void RunFirstRunAssociationPromptIfNeeded();

    // Manual action: prompt and register per-user (HKCU) file associations.
    bool RegisterSupportedFileAssociations(HWND owner);

    // Manual action: elevate and register the ThumbnailProvider DLL via regsvr32.
    void RegisterThumbnailProvider(HWND owner, bool unregister);

    // Returns true when the thumbnail provider is registered.
    bool IsThumbnailProviderRegistered();

    // Window placement persistence (per-user, via INI).
    // Restores/saves the main window's last normal position and show state.
    void LoadWindowPlacement(HWND hwnd);
    void SaveWindowPlacement(HWND hwnd);
}

