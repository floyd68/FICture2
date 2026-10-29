#pragma once

#include "VirtualPath.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace ImageBrowserDeferredActions
{
    struct DeferredAction
    {
        int kind { 0 };
        Floar::VirtualPath path {};
        std::wstring text {};
    };

    // Actions a browser queued for its next deferred-action messages, oldest
    // first. Each queued action posts one message and each message runs one
    // action, so a second action queued before the first ran waits its turn
    // instead of replacing it.
    class DeferredQueue
    {
    public:
        // Far above what input can queue between two message-loop turns.
        static constexpr std::size_t kCapacity = 16;

        // False when the queue is full: the action is dropped, not queued.
        bool Push(DeferredAction action);
        std::optional<DeferredAction> Pop();

        bool Empty() const { return m_actions.empty(); }
        std::size_t Size() const { return m_actions.size(); }

    private:
        std::deque<DeferredAction> m_actions {};
    };

    bool Dispatch(
        const DeferredAction& action,
        const std::function<bool(int)>& noPayloadDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&)>& pathDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&, const std::wstring&)>& pathAndTextDispatcher);
}
