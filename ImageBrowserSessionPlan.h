#pragma once

#include "ImageBrowserSessionPersistence.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// The session rules, without browsers or the file system: what is saved for
// the open viewers and which saved viewers are restored. Reading and writing
// the INI is ImageBrowserSessionPersistence; collecting the viewers, checking
// paths and applying the result are the backplate's.
namespace ImageBrowserSessionPlan
{
    // The save payload for the viewers in registration order (the first is
    // the root). At most `maxViewers` are saved; an entry stands for a viewer
    // whose browser interface was missing and is saved empty.
    ImageBrowserSessionPersistence::SavePayload BuildSavePayload(
        std::vector<ImageBrowserSessionPersistence::SavedViewerEntry> viewers,
        size_t maxViewers,
        bool hasThumbStripHeight,
        float thumbStripHeight,
        std::vector<float> horizontalSplitRatios);

    struct PathChecks
    {
        std::function<bool(const std::wstring&)> fileExists {};
        std::function<bool(const std::wstring&)> folderExists {};
    };

    struct Compaction
    {
        // The dropped viewers that named a path (for logging), in order.
        std::vector<ImageBrowserSessionPersistence::RestoredViewerState> dropped {};
        // Viewers that can still be opened, before clamping (0: not restored).
        size_t survivors { 0 };
    };

    // Keeps the saved viewers that can still be opened, in order: one with a
    // file needs that file (a missing file drops it even if its folder
    // exists); one with only a folder needs the folder; one with neither is
    // dropped silently. The survivors are clamped to `maxViewers` and
    // recorded in `payload.viewers` / `clampedViewerCount`.
    Compaction CompactForRestore(
        ImageBrowserSessionPersistence::RestorePayload& payload,
        size_t maxViewers,
        const PathChecks& checks);
}
