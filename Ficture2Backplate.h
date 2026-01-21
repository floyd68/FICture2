#pragma once

#include "FD2D/Backplate.h"
#include "FD2D/Image.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Ficture2Backplate : public FD2D::Backplate
{
public:
    using FD2D::Backplate::Backplate;

    struct ImageBrowserEvent
    {
        enum class Type
        {
            ThumbStripHeightChanged,
            FileNameSelected,
            ViewTransformChanged,
            ShowNavItemsChanged,
            BackgroundColorChanged,
            FocusedBackgroundColorChanged,
            AlphaCheckerboardChanged
        };

        Type type { Type::ThumbStripHeightChanged };
        FD2D::Wnd* source { nullptr };
        float thumbStripHeight { 0.0f };
        std::wstring fileNameLower {};
        FD2D::Image::ViewTransform viewTransform {};
        bool boolValue { false };
        D2D1_COLOR_F color { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    class EventBus
    {
    public:
        using HandlerId = std::uint64_t;
        using Handler = std::function<void(const ImageBrowserEvent&)>;

        HandlerId Subscribe(Handler handler);
        void Unsubscribe(HandlerId id);
        void Publish(const ImageBrowserEvent& event);

        void RegisterImageBrowser(FD2D::Wnd* browser);
        void UnregisterImageBrowser(FD2D::Wnd* browser);
        std::vector<FD2D::Wnd*> ImageBrowsersSnapshot() const;
        size_t ImageBrowserCount() const;

    private:
        HandlerId m_nextId { 1 };
        std::unordered_map<HandlerId, Handler> m_handlers {};
        std::vector<FD2D::Wnd*> m_imageBrowsers {};
    };

    std::shared_ptr<EventBus> BusPtr() const { return m_eventBus; }
    EventBus& Bus() { return *m_eventBus; }
    const EventBus& Bus() const { return *m_eventBus; }

    void UpdateTitleBarInfo() override;

private:
    std::shared_ptr<EventBus> m_eventBus { std::make_shared<EventBus>() };

protected:
    FD2D::Wnd* FindTargetWnd(const POINT& ptClient) override;
    bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;

private:
    FD2D::Wnd* FindImageBrowserTarget(const POINT& ptClient) const;
};
