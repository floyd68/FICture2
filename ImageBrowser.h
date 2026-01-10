#pragma once

#include <memory>
#include <string>

namespace FD2D
{
    class Wnd;
}

// Core viewer component: supports 1..4 panes for image comparison.
std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, int paneCount = 1, const std::wstring& initialFile = L"");

