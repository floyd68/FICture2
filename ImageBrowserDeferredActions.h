#pragma once

#include "VirtualPath.h"

#include <functional>
#include <string>

namespace ImageBrowserDeferredActions
{
    struct DeferredState
    {
        int kind { 0 };
        VirtualPath path {};
        std::wstring text {};
    };

    struct DeferredSnapshot
    {
        int kind { 0 };
        VirtualPath path {};
        std::wstring text {};
    };

    void Queue(DeferredState& state, int kind, const VirtualPath& path, const std::wstring& text);
    DeferredSnapshot TakeSnapshotAndClear(DeferredState& state, int noneKind);

    bool Dispatch(
        const DeferredSnapshot& snapshot,
        const std::function<bool(int)>& noPayloadDispatcher,
        const std::function<bool(int, const VirtualPath&)>& pathDispatcher,
        const std::function<bool(int, const VirtualPath&, const std::wstring&)>& pathAndTextDispatcher);
}
