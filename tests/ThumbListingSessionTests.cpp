#include "TestHarness.h"

#include "ImageBrowserAsyncThumbLoader.h"

#include <deque>
#include <string>
#include <vector>

// One browser's progressive folder listing: which chunks are merged, when a
// staged update is applied (throttled until the listing completes), and the
// preferred-selection key.

namespace
{
    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\Pictures\\Trip"));
    const Floar::VirtualPath kOtherFolder(std::filesystem::path(L"C:\\Pictures\\Home"));
    constexpr unsigned long long kInterval = ThumbListingSession::kApplyIntervalMs;

    Floar::VirtualFileEntry Entry(const std::filesystem::path& path)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(path), false, 0, 0);
    }

    Floar::VirtualFileEntry File(const wchar_t* name)
    {
        return Entry(kFolder.hostPath / name);
    }

    bool IsImage(const Floar::VirtualPath&)
    {
        return true;
    }

    AsyncThumbListChunkPayload Chunk(unsigned long long requestId, std::vector<Floar::VirtualFileEntry> batch,
        bool completed = false, const Floar::VirtualPath& folder = kFolder, bool showNavItems = true)
    {
        AsyncThumbListChunkPayload chunk {};
        chunk.requestId = requestId;
        chunk.folder = folder;
        chunk.showNavItems = showNavItems;
        chunk.completed = completed;
        chunk.batch = std::move(batch);
        return chunk;
    }

    // A model with a staged listing of kFolder (what a listing start does).
    ThumbItemModel Staged()
    {
        ThumbItemModel model;
        model.StageReset(kFolder, true, &IsImage);
        return model;
    }
}

FIC2_TEST(ThumbListingSession_AStartSupersedesEarlierChunks)
{
    ThumbListingSession session;
    ThumbItemModel model = Staged();
    FIC2_CHECK(!session.Loading());

    const unsigned long long first = session.Start({}, 1000);
    FIC2_CHECK(session.Loading());
    FIC2_CHECK(session.RequestId() == first);
    FIC2_CHECK(session.Merge({ Chunk(first, { File(L"a.png") }) }, kFolder, true, model));
    FIC2_CHECK(session.UpdatePending());

    const unsigned long long second = session.Start({}, 2000);
    FIC2_CHECK(second != first);
    FIC2_CHECK(!session.UpdatePending());
    FIC2_CHECK(!session.Completed());
    FIC2_CHECK(session.StartMs() == 2000);

    // The previous request, another folder, another nav-item setting.
    FIC2_CHECK(!session.Merge({ Chunk(first, { File(L"b.png") }, true) }, kFolder, true, model));
    FIC2_CHECK(!session.Merge({ Chunk(second, { File(L"b.png") }, false, kOtherFolder) }, kFolder, true, model));
    FIC2_CHECK(!session.Merge({ Chunk(second, { File(L"b.png") }, false, kFolder, false) }, kFolder, true, model));
    FIC2_CHECK(!session.UpdatePending());
    FIC2_CHECK(!session.Completed());

    // The folder is compared as a path, case-insensitively.
    const Floar::VirtualPath upper(std::filesystem::path(L"C:\\PICTURES\\TRIP"));
    FIC2_CHECK(session.Merge({ Chunk(second, { File(L"b.png") }, false, upper) }, kFolder, true, model));
    FIC2_CHECK(session.UpdatePending());
}

FIC2_TEST(ThumbListingSession_StagedChunksWaitForTheirCommit)
{
    ThumbListingSession session;
    ThumbItemModel model = Staged();
    const unsigned long long id = session.Start({}, 1000);

    // A chunk that adds nothing (duplicates only) needs no update.
    FIC2_CHECK(session.Merge({ Chunk(id, {}) }, kFolder, true, model));
    FIC2_CHECK(!session.UpdatePending());

    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"a.png"), File(L"b.png") }) }, kFolder, true, model));
    FIC2_CHECK(session.UpdatePending());
    FIC2_CHECK(model.DisplayCount() == 0);   // staged, not committed
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(model.Images().size() == 2);
}

FIC2_TEST(ThumbListingSession_CompletionAppliesAtOnce)
{
    ThumbListingSession session;
    ThumbItemModel model = Staged();
    const unsigned long long id = session.Start({}, 1000);

    // The first update applies at once (nothing was applied yet).
    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"a.png") }) }, kFolder, true, model));
    FIC2_CHECK(session.ShouldApplyNow(1010));
    FIC2_CHECK(!session.Applied(1010));
    FIC2_CHECK(!session.UpdatePending());
    FIC2_CHECK(session.Loading());

    // Within the interval a further chunk waits ...
    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"b.png") }) }, kFolder, true, model));
    FIC2_CHECK(!session.ShouldApplyNow(1010 + kInterval - 1));
    // ... unless the listing completes, which applies (and ends the load) at once.
    FIC2_CHECK(session.Merge({ Chunk(id, {}, true) }, kFolder, true, model));
    FIC2_CHECK(session.Completed());
    FIC2_CHECK(session.UpdatePending());
    FIC2_CHECK(session.ShouldApplyNow(1010 + kInterval - 1));
    FIC2_CHECK(session.Applied(1010 + kInterval - 1));
    FIC2_CHECK(!session.Loading());
    FIC2_CHECK(!session.UpdatePending());
}

FIC2_TEST(ThumbListingSession_ThrottledUpdates)
{
    using Throttle = ThumbListingSession::Throttle;
    ThumbListingSession session;
    ThumbItemModel model = Staged();
    const unsigned long long id = session.Start({}, 1000);
    FIC2_CHECK(session.ThrottledUpdate(1000) == Throttle::None);   // nothing pending

    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"a.png") }) }, kFolder, true, model));
    (void)session.Applied(1000);
    FIC2_CHECK(session.ThrottledUpdate(1000) == Throttle::None);

    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"b.png") }) }, kFolder, true, model));
    FIC2_CHECK(session.ThrottledUpdate(1000 + kInterval - 1) == Throttle::Wait);
    FIC2_CHECK(session.ThrottledUpdate(1000 + kInterval) == Throttle::Apply);

    // A completed listing is applied by the chunk path, not the throttle.
    FIC2_CHECK(session.Merge({ Chunk(id, {}, true) }, kFolder, true, model));
    FIC2_CHECK(session.ThrottledUpdate(1000 + kInterval) == Throttle::None);
}

FIC2_TEST(ThumbListingSession_PreferredKeyResolvedOncePerListing)
{
    // Two listed spellings of one file: equal as paths, different lookup keys.
    const Floar::VirtualFileEntry backslashes = Entry(L"C:\\Pictures\\Trip\\img.png");
    const Floar::VirtualFileEntry mixed = Entry(L"C:\\Pictures/Trip\\img.png");
    const Floar::VirtualPath prefer(std::filesystem::path(L"C:/Pictures/Trip/IMG.png"));
    FIC2_REQUIRE(ThumbItemModel::LookupKeyFor(backslashes.path) != ThumbItemModel::LookupKeyFor(mixed.path));
    FIC2_REQUIRE(ThumbItemModel::LookupKeyFor(prefer) != ThumbItemModel::LookupKeyFor(backslashes.path));
    FIC2_REQUIRE(ThumbItemModel::LookupKeyFor(prefer) != ThumbItemModel::LookupKeyFor(mixed.path));

    ThumbItemModel model;
    model.Reset(kFolder, false, &IsImage);
    (void)model.Merge({ File(L"a.png"), backslashes, mixed });
    FIC2_REQUIRE(model.DisplayCount() == 3);   // a.png, then both spellings in arrival order

    ThumbListingSession session;
    const unsigned long long id = session.Start(prefer, 1000);
    FIC2_CHECK(session.PreferSelectPath() == prefer);
    FIC2_CHECK(session.PreferredIndex(model, prefer) == 0u);   // not resolved yet

    // The first listed match wins; later matches do not replace it.
    ThumbItemModel staged = Staged();
    FIC2_CHECK(session.Merge({ Chunk(id, { File(L"a.png"), mixed }) }, kFolder, true, staged));
    FIC2_CHECK(session.Merge({ Chunk(id, { backslashes }) }, kFolder, true, staged));
    FIC2_CHECK(session.PreferredIndex(model, prefer) == 2u);

    // A new listing resolves again.
    (void)session.Start(prefer, 2000);
    session.ResolvePreferKey(prefer, { backslashes });
    FIC2_CHECK(session.PreferredIndex(model, prefer) == 1u);
    session.ResetPreferKey();
    FIC2_CHECK(session.PreferredIndex(model, prefer) == 0u);

    // A path listed with its own key needs no resolution; an empty one is 0.
    FIC2_CHECK(session.PreferredIndex(model, backslashes.path) == 1u);
    FIC2_CHECK(session.PreferredIndex(model, mixed.path) == 2u);
    FIC2_CHECK(session.PreferredIndex(model, {}) == 0u);
}
