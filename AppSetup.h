#pragma once

#include <string>

namespace FICture2App
{
    // %LOCALAPPDATA%\FICture2\FICture2.ini
    std::wstring GetIniFilePath();

    // First-run is defined as "INI does not exist".
    // If first-run, prompts the user and (optionally) registers per-user (HKCU) file associations.
    // Always creates the INI (best-effort) so the prompt is shown only once.
    void RunFirstRunAssociationPromptIfNeeded();
}

