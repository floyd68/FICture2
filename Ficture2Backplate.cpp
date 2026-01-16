#include "Ficture2Backplate.h"

#include <algorithm>

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
