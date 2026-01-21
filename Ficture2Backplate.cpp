#include "Ficture2Backplate.h"

#include "FD2D/Core.h"

#include <algorithm>
#include <shellapi.h>

Ficture2Backplate::EventBus::HandlerId Ficture2Backplate::EventBus::Subscribe(Handler handler)
{
    if (!handler)
    {
        return 0;
    }

    const HandlerId id = m_nextId++;
    m_handlers.emplace(id, std::move(handler));
    return id;
}

void Ficture2Backplate::EventBus::Unsubscribe(HandlerId id)
{
    if (id == 0)
    {
        return;
    }

    m_handlers.erase(id);
}

void Ficture2Backplate::EventBus::Publish(const ImageBrowserEvent& event)
{
    for (const auto& pair : m_handlers)
    {
        if (pair.second)
        {
            pair.second(event);
        }
    }
}

void Ficture2Backplate::EventBus::RegisterImageBrowser(FD2D::Wnd* browser)
{
    if (browser == nullptr)
    {
        return;
    }

    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
    {
        return;
    }

    m_imageBrowsers.push_back(browser);
}

void Ficture2Backplate::EventBus::UnregisterImageBrowser(FD2D::Wnd* browser)
{
    if (browser == nullptr)
    {
        return;
    }

    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
    {
        m_imageBrowsers.erase(it);
    }
}

std::vector<FD2D::Wnd*> Ficture2Backplate::EventBus::ImageBrowsersSnapshot() const
{
    return m_imageBrowsers;
}

size_t Ficture2Backplate::EventBus::ImageBrowserCount() const
{
    return m_imageBrowsers.size();
}

FD2D::Wnd* Ficture2Backplate::FindImageBrowserTarget(const POINT& ptClient) const
{
    if (!m_eventBus)
    {
        return nullptr;
    }

    FD2D::Wnd* best = nullptr;
    float bestArea = 0.0f;

    const auto browsers = m_eventBus->ImageBrowsersSnapshot();
    for (auto* b : browsers)
    {
        if (b == nullptr)
        {
            continue;
        }

        const D2D1_RECT_F r = b->LayoutRect();
        if (ptClient.x < r.left || ptClient.x > r.right || ptClient.y < r.top || ptClient.y > r.bottom)
        {
            continue;
        }

        const float w = (std::max)(0.0f, r.right - r.left);
        const float h = (std::max)(0.0f, r.bottom - r.top);
        const float area = w * h;

        // Choose the smallest containing rect (most specific/deepest pane).
        if (best == nullptr || area < bestArea)
        {
            best = b;
            bestArea = area;
        }
    }

    return best;
}

FD2D::Wnd* Ficture2Backplate::FindTargetWnd(const POINT& ptClient)
{
    return FindImageBrowserTarget(ptClient);
}

bool Ficture2Backplate::HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    UNREFERENCED_PARAMETER(hWnd);

    switch (message)
    {
    case WM_CREATE:
    {
        UpdateTitleBarInfo();
        DragAcceptFiles(m_window, TRUE);
        (void)EnsureDropTargetRegistered();
        result = 0;
        return true;
    }
    case WM_DROPFILES:
    {
        const HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        if (hDrop == nullptr)
        {
            result = 0;
            return true;
        }

        wchar_t pathBuf[MAX_PATH] {};
        const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (fileCount == 0)
        {
            DragFinish(hDrop);
            result = 0;
            return true;
        }

        POINT pt {};
        (void)DragQueryPoint(hDrop, &pt); // client coordinates

        const UINT cch = DragQueryFileW(hDrop, 0, pathBuf, static_cast<UINT>(std::size(pathBuf)));
        DragFinish(hDrop);

        if (cch == 0)
        {
            result = 0;
            return true;
        }

        const std::wstring path(pathBuf);

        // Route to UI tree: hit-test top-level children and allow Wnd overrides to handle.
        for (auto& pair : m_children)
        {
            if (pair.second && pair.second->OnFileDrop(path, pt))
            {
                break;
            }
        }

        result = 0;
        return true;
    }
    case WM_DESTROY:
    {
        HandleFileDragLeave();
        UnregisterDropTarget();
        break;
    }
    default:
        break;
    }

    return Backplate::HandleMessage(hWnd, message, wParam, lParam, result);
}

void Ficture2Backplate::UpdateTitleBarInfo()
{
    if (m_window == nullptr)
    {
        return;
    }

    // Get current window title
    wchar_t currentTitle[256] = {};
    GetWindowTextW(m_window, currentTitle, static_cast<int>(std::size(currentTitle)));

    // Extract base title (remove existing info if present)
    std::wstring baseTitle = currentTitle;
    size_t infoPos = baseTitle.find(L" [");
    if (infoPos != std::wstring::npos)
    {
        baseTitle = baseTitle.substr(0, infoPos);
    }
    if (baseTitle.empty())
    {
        baseTitle = L"FICture2";
    }

    // Get Direct2D version
    const char* d2dVersionStr = FD2D::Core::GetD2DVersionString();

    // Extract version number (e.g., "Direct2D 1.3 (Windows 10+)" -> "1.3")
    std::string d2dVersionA = d2dVersionStr ? d2dVersionStr : "";
    size_t versionPos = d2dVersionA.find("1.");
    std::string versionNum = "1.0";
    if (versionPos != std::string::npos)
    {
        size_t endPos = versionPos + 3; // "1.x"
        if (endPos > d2dVersionA.length())
        {
            endPos = d2dVersionA.length();
        }
        versionNum = d2dVersionA.substr(versionPos, endPos - versionPos);
    }

    // Convert to wide string
    size_t len = versionNum.length();
    std::wstring versionNumW(len + 1, L'\0');
    mbstowcs_s(nullptr, &versionNumW[0], len + 1, versionNum.c_str(), len);
    versionNumW.resize(len);

    // Check if using D3D11 renderer
    bool usingD3D11 = (m_rendererId.empty() || m_rendererId == L"d3d11_swapchain");

    // Build new title with renderer info on the right
    wchar_t newTitle[512];
    if (usingD3D11)
    {
        swprintf_s(newTitle, L"%ls [Renderer: D3D11 | D2D %ls]", baseTitle.c_str(), versionNumW.c_str());
    }
    else
    {
        swprintf_s(newTitle, L"%ls [Renderer: D2D | D2D %ls]", baseTitle.c_str(), versionNumW.c_str());
    }

    SetWindowTextW(m_window, newTitle);
}
