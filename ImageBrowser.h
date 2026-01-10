#pragma once

#include <memory>
#include <string>

namespace FD2D
{
    class Wnd;
}

// Core viewer component: supports 1..4 panes for image comparison.
std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, int paneCount = 1, const std::wstring& initialFile = L"");

// IPC hook:
// Called by the first instance when a second launch sends a file path.
// Returns true if the request started compare mode (split view), false if ignored.
bool ImageBrowser_TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath);

