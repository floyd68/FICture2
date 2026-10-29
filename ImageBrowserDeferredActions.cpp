#include "ImageBrowserDeferredActions.h"

#include <utility>

namespace ImageBrowserDeferredActions
{
    bool DeferredQueue::Push(DeferredAction action)
    {
        if (m_actions.size() >= kCapacity)
        {
            return false;
        }
        m_actions.push_back(std::move(action));
        return true;
    }

    std::optional<DeferredAction> DeferredQueue::Pop()
    {
        if (m_actions.empty())
        {
            return std::nullopt;
        }
        DeferredAction action = std::move(m_actions.front());
        m_actions.pop_front();
        return action;
    }

    bool Dispatch(
        const DeferredAction& action,
        const std::function<bool(int)>& noPayloadDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&)>& pathDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&, const std::wstring&)>& pathAndTextDispatcher)
    {
        if (noPayloadDispatcher && noPayloadDispatcher(action.kind))
        {
            return true;
        }
        if (pathDispatcher && pathDispatcher(action.kind, action.path))
        {
            return true;
        }
        if (pathAndTextDispatcher && pathAndTextDispatcher(action.kind, action.path, action.text))
        {
            return true;
        }
        return false;
    }
}
