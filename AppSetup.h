#pragma once

#include <string>

// Forward declare Win32 types without pulling in <windows.h> from this header.
struct HWND__;
using HWND = HWND__*;
struct tagRECT;
using RECT = tagRECT;

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

    // Manual action: prompt and remove per-user associations written by Register.
    bool UnregisterSupportedFileAssociations(HWND owner);

    // True when the per-user ProgID from a previous registration is present.
    // Used to pick which action the app context menu offers.
    bool AreFileAssociationsRegistered();

    // Manual action: elevate and register the ThumbnailProvider DLL via regsvr32.
    void RegisterThumbnailProvider(HWND owner, bool unregister);

    // Returns true when the thumbnail provider is registered.
    bool IsThumbnailProviderRegistered();

    // Window placement persistence (per-user, via INI).

    // Reads the saved normal window rect and show command from the INI file.
    // outRect is clamped to the nearest monitor's work area.
    // outShowCmd is SW_SHOWNORMAL or SW_SHOWMAXIMIZED.
    // Returns false if no placement has been saved yet (first run).
    bool ReadWindowPlacement(RECT& outRect, int& outShowCmd);

    // Saves the current window position and show state to the INI file.
    void SaveWindowPlacement(HWND hwnd);

    // Legacy: applies the saved placement to an already-created window.
    // Prefer ReadWindowPlacement + passing position to CreateWindowed instead.
    void LoadWindowPlacement(HWND hwnd);
}

