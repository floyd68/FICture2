#include "ImageBrowserSessionPlan.h"

#include <algorithm>
#include <utility>

namespace ImageBrowserSessionPlan
{
    ImageBrowserSessionPersistence::SavePayload BuildSavePayload(
        std::vector<ImageBrowserSessionPersistence::SavedViewerEntry> viewers,
        size_t maxViewers,
        bool hasThumbStripHeight,
        float thumbStripHeight,
        std::vector<float> horizontalSplitRatios)
    {
        if (viewers.size() > maxViewers)
        {
            viewers.resize(maxViewers);
        }

        ImageBrowserSessionPersistence::SavePayload payload {};
        payload.viewerCount = static_cast<int>(viewers.size());
        payload.viewers = std::move(viewers);
        payload.hasThumbStripHeight = hasThumbStripHeight;
        payload.thumbStripHeight = thumbStripHeight;
        payload.horizontalSplitRatios = std::move(horizontalSplitRatios);
        return payload;
    }

    Compaction CompactForRestore(
        ImageBrowserSessionPersistence::RestorePayload& payload,
        size_t maxViewers,
        const PathChecks& checks)
    {
        std::vector<ImageBrowserSessionPersistence::RestoredViewerState> valid;
        Compaction result {};
        auto& dropped = result.dropped;
        valid.reserve(payload.viewers.size());
        for (const auto& viewer : payload.viewers)
        {
            if (!viewer.filePath.empty())
            {
                if (checks.fileExists && checks.fileExists(viewer.filePath))
                {
                    valid.push_back(viewer);
                }
                else
                {
                    dropped.push_back(viewer);
                }
            }
            else if (!viewer.folderPath.empty())
            {
                if (checks.folderExists && checks.folderExists(viewer.folderPath))
                {
                    valid.push_back(viewer);
                }
                else
                {
                    dropped.push_back(viewer);
                }
            }
        }

        result.survivors = valid.size();
        if (valid.size() > maxViewers)
        {
            valid.resize(maxViewers);
        }
        payload.clampedViewerCount = static_cast<int>(valid.size());
        payload.viewers = std::move(valid);
        return result;
    }
}
