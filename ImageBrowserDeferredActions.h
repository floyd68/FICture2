#pragma once

#include "VirtualPath.h"

#include <functional>
#include <string>

namespace ImageBrowserDeferredActions
{
    struct DeferredState
    {
        int kind { 0 };
        Floar::VirtualPath path {};
        std::wstring text {};
    };

    struct DeferredSnapshot
    {
        int kind { 0 };
        Floar::VirtualPath path {};
        std::wstring text {};
    };

    void Queue(DeferredState& state, int kind, const Floar::VirtualPath& path, const std::wstring& text);
    DeferredSnapshot TakeSnapshotAndClear(DeferredState& state, int noneKind);

    bool Dispatch(
        const DeferredSnapshot& snapshot,
        const std::function<bool(int)>& noPayloadDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&)>& pathDispatcher,
        const std::function<bool(int, const Floar::VirtualPath&, const std::wstring&)>& pathAndTextDispatcher);
}
