#pragma once

#include <string>
#include <vector>

namespace ImageBrowserSessionPersistence
{
    struct SavedViewerEntry
    {
        std::wstring displayedFile {};
        std::wstring currentFolder {};
    };

    struct SavePayload
    {
        int viewerCount { 0 };
        std::vector<SavedViewerEntry> viewers {};
        bool hasThumbStripHeight { false };
        float thumbStripHeight { 0.0f };
        std::vector<float> horizontalSplitRatios {};
    };

    struct RestoredViewerState
    {
        std::wstring filePath {};
        std::wstring folderPath {};
    };

    struct RestorePayload
    {
        int viewerCount { 0 };
        int clampedViewerCount { 0 };
        bool hasThumbStripHeight { false };
        float thumbStripHeight { 0.0f };
        std::vector<RestoredViewerState> viewers {};
        std::vector<float> horizontalSplitRatios {};
    };

    void SaveToIni(const std::wstring& iniFile, const SavePayload& payload);
    bool TryRestoreFromIni(const std::wstring& iniFile, RestorePayload& outPayload);
}
