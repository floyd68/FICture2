#include "ImageBrowserDeferredActions.h"

namespace ImageBrowserDeferredActions
{
    void Queue(DeferredState& state, int kind, const VirtualPath& path, const std::wstring& text)
    {
        state.kind = kind;
        state.path = path;
        state.text = text;
    }

    DeferredSnapshot TakeSnapshotAndClear(DeferredState& state, int noneKind)
    {
        DeferredSnapshot snapshot {};
        snapshot.kind = state.kind;
        snapshot.path = state.path;
        snapshot.text = state.text;

        state.kind = noneKind;
        state.path = VirtualPath();
        state.text.clear();
        return snapshot;
    }

    bool Dispatch(
        const DeferredSnapshot& snapshot,
        const std::function<bool(int)>& noPayloadDispatcher,
        const std::function<bool(int, const VirtualPath&)>& pathDispatcher,
        const std::function<bool(int, const VirtualPath&, const std::wstring&)>& pathAndTextDispatcher)
    {
        if (noPayloadDispatcher && noPayloadDispatcher(snapshot.kind))
        {
            return true;
        }
        if (pathDispatcher && pathDispatcher(snapshot.kind, snapshot.path))
        {
            return true;
        }
        if (pathAndTextDispatcher && pathAndTextDispatcher(snapshot.kind, snapshot.path, snapshot.text))
        {
            return true;
        }
        return false;
    }
}
