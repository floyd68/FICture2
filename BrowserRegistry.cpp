#include "BrowserRegistry.h"

#include <algorithm>

void BrowserRegistry::RegisterImageBrowser(FD2D::Wnd* browser)
{
    if (browser == nullptr)
        return;

    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
        return;

    m_imageBrowsers.push_back(browser);
}

void BrowserRegistry::UnregisterImageBrowser(FD2D::Wnd* browser)
{
    const auto it = std::find(m_imageBrowsers.begin(), m_imageBrowsers.end(), browser);
    if (it != m_imageBrowsers.end())
        m_imageBrowsers.erase(it);
}
