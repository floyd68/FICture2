#include "TestHarness.h"

#include "ImageBrowserSessionPlan.h"

#include <set>
#include <string>
#include <vector>

// The session rules without browsers or the file system: the save payload
// (clamped, empty entries kept) and the restore compaction with injected
// path checks.

namespace
{
    using ImageBrowserSessionPersistence::RestoredViewerState;
    using ImageBrowserSessionPersistence::RestorePayload;
    using ImageBrowserSessionPersistence::SavedViewerEntry;

    ImageBrowserSessionPlan::PathChecks Checks(std::set<std::wstring> files, std::set<std::wstring> folders)
    {
        ImageBrowserSessionPlan::PathChecks checks {};
        checks.fileExists = [files](const std::wstring& p) { return files.count(p) != 0; };
        checks.folderExists = [folders](const std::wstring& p) { return folders.count(p) != 0; };
        return checks;
    }

    RestorePayload Saved(std::vector<RestoredViewerState> viewers)
    {
        RestorePayload payload {};
        payload.viewerCount = static_cast<int>(viewers.size());
        payload.viewers = std::move(viewers);
        return payload;
    }

    std::vector<std::wstring> Files(const RestorePayload& payload)
    {
        std::vector<std::wstring> out;
        for (const auto& viewer : payload.viewers)
        {
            out.push_back(viewer.filePath.empty() ? L"dir:" + viewer.folderPath : viewer.filePath);
        }
        return out;
    }
}

FIC2_TEST(ImageBrowserSessionPlan_SaveClampsAndKeepsEmptyEntries)
{
    std::vector<SavedViewerEntry> viewers;
    viewers.push_back({ L"a.png", L"f1" });
    viewers.push_back({});                       // a browser without its interface
    for (int i = 0; i < 8; ++i)
    {
        viewers.push_back({ L"x" + std::to_wstring(i) + L".png", L"f2" });
    }

    const auto payload = ImageBrowserSessionPlan::BuildSavePayload(viewers, 8, true, 140.0f, { 0.25f, 0.5f });
    FIC2_CHECK(payload.viewerCount == 8);
    FIC2_REQUIRE(payload.viewers.size() == 8);
    FIC2_CHECK(payload.viewers[0].displayedFile == L"a.png" && payload.viewers[0].currentFolder == L"f1");
    FIC2_CHECK(payload.viewers[1].displayedFile.empty() && payload.viewers[1].currentFolder.empty());
    FIC2_CHECK(payload.viewers[7].displayedFile == L"x5.png");
    FIC2_CHECK(payload.hasThumbStripHeight && payload.thumbStripHeight == 140.0f);
    FIC2_CHECK(payload.horizontalSplitRatios == std::vector<float>({ 0.25f, 0.5f }));

    const auto few = ImageBrowserSessionPlan::BuildSavePayload({ { L"a.png", L"f1" } }, 8, false, 0.0f, {});
    FIC2_CHECK(few.viewerCount == 1 && few.viewers.size() == 1);
    FIC2_CHECK(!few.hasThumbStripHeight);
}

FIC2_TEST(ImageBrowserSessionPlan_RestoreKeepsWhatStillExists)
{
    RestorePayload payload = Saved({
        { L"a.png", L"f1" },
        { L"", L"f2" },
        { L"b.png", L"" },
    });
    const auto compaction = ImageBrowserSessionPlan::CompactForRestore(
        payload, 8, Checks({ L"a.png", L"b.png" }, { L"f2" }));
    FIC2_CHECK(compaction.dropped.empty());
    FIC2_CHECK(compaction.survivors == 3);
    FIC2_CHECK(payload.clampedViewerCount == 3);
    FIC2_CHECK(Files(payload) == std::vector<std::wstring>({ L"a.png", L"dir:f2", L"b.png" }));
}

FIC2_TEST(ImageBrowserSessionPlan_RestoreDropsMissingPaths)
{
    RestorePayload payload = Saved({
        { L"gone.png", L"f1" },   // the file is gone: dropped although f1 exists
        { L"", L"gone-dir" },     // the folder is gone
        { L"", L"" },             // nothing to restore: dropped silently
        { L"b.png", L"f1" },
    });
    const auto compaction = ImageBrowserSessionPlan::CompactForRestore(
        payload, 8, Checks({ L"b.png" }, { L"f1" }));
    FIC2_REQUIRE(compaction.dropped.size() == 2);
    FIC2_CHECK(compaction.dropped[0].filePath == L"gone.png");
    FIC2_CHECK(compaction.dropped[1].filePath.empty() && compaction.dropped[1].folderPath == L"gone-dir");
    FIC2_CHECK(compaction.survivors == 1);
    FIC2_CHECK(payload.clampedViewerCount == 1);
    FIC2_CHECK(Files(payload) == std::vector<std::wstring>({ L"b.png" }));
}

FIC2_TEST(ImageBrowserSessionPlan_RestoreKeepsOrderAndClamps)
{
    std::vector<RestoredViewerState> viewers;
    std::set<std::wstring> files;
    for (int i = 0; i < 10; ++i)
    {
        const std::wstring name = L"v" + std::to_wstring(i) + L".png";
        viewers.push_back({ name, L"" });
        if (i != 2)
        {
            files.insert(name);
        }
    }
    RestorePayload payload = Saved(viewers);
    const auto compaction = ImageBrowserSessionPlan::CompactForRestore(payload, 8, Checks(files, {}));
    FIC2_CHECK(compaction.survivors == 9);          // before clamping
    FIC2_CHECK(payload.clampedViewerCount == 8);
    FIC2_CHECK(Files(payload) == std::vector<std::wstring>(
        { L"v0.png", L"v1.png", L"v3.png", L"v4.png", L"v5.png", L"v6.png", L"v7.png", L"v8.png" }));
}

FIC2_TEST(ImageBrowserSessionPlan_RestoreWithNoSurvivors)
{
    RestorePayload payload = Saved({ { L"gone.png", L"" }, { L"", L"" } });
    const auto compaction = ImageBrowserSessionPlan::CompactForRestore(payload, 8, Checks({}, {}));
    FIC2_CHECK(compaction.survivors == 0);
    FIC2_CHECK(compaction.dropped.size() == 1);
    FIC2_CHECK(payload.viewers.empty());
    FIC2_CHECK(payload.clampedViewerCount == 0);

    // No checks at all: nothing can be shown to exist.
    RestorePayload unchecked = Saved({ { L"a.png", L"" } });
    FIC2_CHECK(ImageBrowserSessionPlan::CompactForRestore(unchecked, 8, {}).survivors == 0);
}
