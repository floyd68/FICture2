#include "TestHarness.h"

#include "ImageAwareVfs.h"
#include "ImageBrowserThumbModel.h"

#include <string>
#include <vector>

// The thumbnail item model: classification, de-duplication and display order,
// merged incrementally from folder-listing chunks.

namespace
{
    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\Pictures\\Trip"));

    Floar::VirtualFileEntry File(const wchar_t* name)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(kFolder.hostPath / name), false, 0, 0);
    }

    Floar::VirtualFileEntry Dir(const wchar_t* name)
    {
        return Floar::VirtualFileEntry(Floar::VirtualPath(kFolder.hostPath / name), true, 0, 0);
    }

    bool IsTestImage(const Floar::VirtualPath& path)
    {
        const std::wstring ext = CommonUtil::ToLower(path.GetExtension());
        return ext == L".png" || ext == L".dds";
    }

    std::vector<std::wstring> Names(const std::vector<ThumbModelEntry>& entries)
    {
        std::vector<std::wstring> names;
        for (const auto& entry : entries)
        {
            names.push_back(entry.path.GetFilename());
        }
        return names;
    }

    std::vector<Floar::VirtualFileEntry> Listing()
    {
        return {
            File(L"b.png"), Dir(L"Zeta"), File(L"readme.txt"), File(L"A.png"),
            File(L"pack.zip"), Dir(L"alpha"), File(L"c.dds"),
        };
    }
}

FIC2_TEST(ThumbItemModel_OrderMatchesTheStrip)
{
    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    FIC2_CHECK(model.Merge(Listing()) == 6);   // readme.txt is not an image

    FIC2_REQUIRE(model.Up().has_value());
    FIC2_CHECK(model.Up()->kind == ThumbItemKind::Up);
    FIC2_CHECK(model.Up()->path == kFolder.GetParent());
    // Folders and archives by lower-cased name, then images by lower-cased name.
    FIC2_CHECK(Names(model.Folders()) == (std::vector<std::wstring> { L"alpha", L"pack.zip", L"Zeta" }));
    FIC2_CHECK(model.Folders()[1].isArchive);
    FIC2_CHECK(!model.Folders()[0].isArchive);
    FIC2_CHECK(Names(model.Images()) == (std::vector<std::wstring> { L"A.png", L"b.png", L"c.dds" }));
    FIC2_CHECK(model.DisplayCount() == 7);
    FIC2_CHECK(model.ListedCount() == 7);
}

FIC2_TEST(ThumbItemModel_ChunksMergeIncrementally)
{
    ThumbItemModel whole;
    whole.Reset(kFolder, true, &IsTestImage);
    (void)whole.Merge(Listing());

    // The same listing in out-of-order chunks, with duplicates across chunks.
    ThumbItemModel chunked;
    chunked.Reset(kFolder, true, &IsTestImage);
    FIC2_CHECK(chunked.Merge({ File(L"c.dds"), Dir(L"Zeta") }) == 2);
    FIC2_CHECK(chunked.Merge({ File(L"b.png"), File(L"C.DDS"), File(L"readme.txt") }) == 1);   // C.DDS: duplicate
    FIC2_CHECK(chunked.Merge({ Dir(L"alpha"), File(L"pack.zip"), File(L"A.png"), Dir(L"zeta") }) == 3);
    FIC2_CHECK(chunked.Merge({}) == 0);

    FIC2_CHECK(Names(chunked.Folders()) == Names(whole.Folders()));
    FIC2_CHECK(Names(chunked.Images()) == Names(whole.Images()));
    FIC2_CHECK(chunked.DisplayCount() == whole.DisplayCount());
    FIC2_CHECK(chunked.ListedCount() == 9);

    // A later chunk inserts in the middle of the existing order.
    FIC2_CHECK(chunked.Merge({ File(L"B2.png"), File(L"aa.png") }) == 2);
    FIC2_CHECK(Names(chunked.Images()) == (std::vector<std::wstring> { L"A.png", L"aa.png", L"b.png", L"B2.png", L"c.dds" }));
}

FIC2_TEST(ThumbItemModel_CaseVariantsAreOneItem)
{
    // As before the model: paths that differ only in case are one item (the
    // first listed wins), also inside archives.
    const Floar::VirtualPath archive(std::filesystem::path(L"C:\\Pictures\\pack.zip"));
    ThumbItemModel model;
    model.Reset(archive, false, &IsTestImage);
    FIC2_CHECK(model.Merge({
        Floar::VirtualFileEntry(Floar::VirtualPath(archive.hostPath, L"x.png"), false, 0, 0),
        Floar::VirtualFileEntry(Floar::VirtualPath(archive.hostPath, L"b.png"), false, 0, 0),
    }) == 2);
    FIC2_CHECK(model.Merge({ Floar::VirtualFileEntry(Floar::VirtualPath(archive.hostPath, L"X.PNG"), false, 0, 0) }) == 0);
    FIC2_CHECK(Names(model.Images()) == (std::vector<std::wstring> { L"b.png", L"x.png" }));
}

FIC2_TEST(ThumbItemModel_NavItemsHiddenOrAtRoot)
{
    ThumbItemModel hidden;
    hidden.Reset(kFolder, false, &IsTestImage);
    (void)hidden.Merge(Listing());
    FIC2_CHECK(!hidden.Up().has_value());
    FIC2_CHECK(hidden.Folders().size() == 3);   // kept, but not displayed
    FIC2_CHECK(hidden.DisplayCount() == 3);

    ThumbItemModel root;
    root.Reset(Floar::VirtualPath(std::filesystem::path(L"C:\\")), true, &IsTestImage);
    FIC2_CHECK(!root.Up().has_value());

    // Reset clears everything, including the de-duplication keys.
    hidden.Reset(kFolder, true, &IsTestImage);
    FIC2_CHECK(hidden.DisplayCount() == 1);   // Up only
    FIC2_CHECK(hidden.Merge({ File(L"A.png") }) == 1);
}

FIC2_TEST(ThumbItemModel_AspectsSurviveASameFolderRebuild)
{
    // A rebuild of the same folder (for example toggling nav items) reuses the
    // tiles, which do not report their aspect again: the model keeps it.
    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    (void)model.Merge(Listing());
    const std::wstring key = ThumbItemModel::LookupKeyFor(File(L"b.png").path);
    FIC2_CHECK(model.SetAspect(key, 2.0f));
    FIC2_CHECK(!model.SetAspect(key, 2.0f));   // unchanged
    FIC2_CHECK(!model.SetAspect(key, 0.0f));   // not an aspect

    model.Reset(Floar::VirtualPath(std::filesystem::path(L"c:/pictures/trip")), false, &IsTestImage);   // same folder, other spelling
    FIC2_CHECK(model.AspectOf(key) == 2.0f);
    (void)model.Merge(Listing());
    FIC2_CHECK(model.AspectOf(key) == 2.0f);

    // Another folder starts without aspects.
    model.Reset(Floar::VirtualPath(std::filesystem::path(L"C:\\Pictures\\Other")), true, &IsTestImage);
    FIC2_CHECK(model.AspectOf(key) == 0.0f);
}

FIC2_TEST(ThumbItemModel_AtFollowsTheDisplayOrder)
{
    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    (void)model.Merge(Listing());
    std::vector<std::wstring> names;
    for (size_t i = 0; i < model.DisplayCount(); ++i)
    {
        names.push_back(model.At(i).path.GetFilename());
    }
    FIC2_CHECK(model.At(0).kind == ThumbItemKind::Up);
    FIC2_CHECK(model.At(0).lookupKey == ThumbItemModel::LookupKeyFor(kFolder.GetParent()));
    FIC2_CHECK(model.At(1).kind == ThumbItemKind::Folder);
    FIC2_CHECK(model.At(4).kind == ThumbItemKind::Image);
    names.erase(names.begin());   // the Up item's name is the parent's
    FIC2_CHECK(names == (std::vector<std::wstring> { L"alpha", L"pack.zip", L"Zeta", L"A.png", L"b.png", L"c.dds" }));

    // Without nav items, index 0 is the first image.
    ThumbItemModel hidden;
    hidden.Reset(kFolder, false, &IsTestImage);
    (void)hidden.Merge(Listing());
    FIC2_CHECK(hidden.At(0).path.GetFilename() == L"A.png");
    FIC2_CHECK(hidden.At(2).path.GetFilename() == L"c.dds");
}

FIC2_TEST(ThumbItemModel_StagedListingIsDisplayedOnCommit)
{
    // A progressive listing stages chunks; the displayed items (and their
    // display indices) change only when the strip commits them.
    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    (void)model.Merge(Listing());
    const std::wstring key = ThumbItemModel::LookupKeyFor(File(L"b.png").path);
    FIC2_CHECK(model.SetAspect(key, 2.0f));
    FIC2_CHECK(!model.Commit());   // nothing staged

    // Staged items in the same folder: de-duplicated now, displayed later.
    FIC2_CHECK(model.Stage({ File(L"aa.png"), File(L"B.PNG") }) == 1);
    FIC2_CHECK(model.Stage({ File(L"AA.png") }) == 0);   // duplicate of a staged item
    FIC2_CHECK(Names(model.Images()) == (std::vector<std::wstring> { L"A.png", L"b.png", L"c.dds" }));
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(Names(model.Images()) == (std::vector<std::wstring> { L"A.png", L"aa.png", L"b.png", L"c.dds" }));

    // A staged reset to another folder keeps the displayed items and aspects.
    const Floar::VirtualPath other(std::filesystem::path(L"C:\\Pictures\\Other"));
    model.StageReset(other, false, &IsTestImage);
    FIC2_CHECK(model.Stage({ Floar::VirtualFileEntry(Floar::VirtualPath(other.hostPath / L"z.png"), false, 0, 0) }) == 1);
    FIC2_CHECK(model.Stage({ File(L"A.png") }) == 1);   // de-duplicated against the new listing only
    FIC2_CHECK(model.ListedCount() == 2);
    FIC2_CHECK(model.DisplayCount() == 8);
    FIC2_CHECK(model.Folder() == kFolder);
    FIC2_CHECK(model.At(0).kind == ThumbItemKind::Up);
    FIC2_CHECK(model.AspectOf(key) == 2.0f);

    FIC2_CHECK(model.Commit());
    FIC2_CHECK(model.Folder() == other);
    FIC2_CHECK(!model.ShowNavItems());
    FIC2_CHECK(!model.Up().has_value());
    FIC2_CHECK(Names(model.Images()) == (std::vector<std::wstring> { L"A.png", L"z.png" }));
    FIC2_CHECK(model.AspectOf(key) == 0.0f);

    // A staged reset with nothing staged still replaces the display on commit.
    model.StageReset(kFolder, true, &IsTestImage);
    FIC2_CHECK(model.DisplayCount() == 2);
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(model.DisplayCount() == 1);   // Up only
}

FIC2_TEST(ThumbItemModel_KeepsTheListedSourceIdentity)
{
    // Each item keeps the identity it was listed with; a relisting of the same
    // folder (refresh) replaces it, so a file replaced at the same path is
    // noticed then. Entries listed without one are unknown.
    const auto identity = [](uint64_t size, uint64_t writeTime)
    {
        ImageSourceIdentity id {};
        id.size = size;
        id.writeTime = writeTime;
        id.known = true;
        return id;
    };
    const std::wstring key = ThumbItemModel::LookupKeyFor(File(L"b.png").path);
    const auto itemIdentity = [&](const ThumbItemModel& model) -> ImageSourceIdentity
    {
        for (const ThumbModelEntry& entry : model.Images())
        {
            if (entry.lookupKey == key)
            {
                return entry.identity;
            }
        }
        return {};
    };

    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    FIC2_CHECK(model.Merge({ File(L"a.png"), File(L"b.png") }, { identity(10, 100), identity(20, 200) }) == 2);
    FIC2_CHECK(itemIdentity(model) == identity(20, 200));
    FIC2_CHECK(model.Images()[0].identity == identity(10, 100));

    // A duplicate keeps the identity it was first listed with.
    FIC2_CHECK(model.Merge({ File(L"B.png") }, { identity(30, 300) }) == 0);
    FIC2_CHECK(itemIdentity(model) == identity(20, 200));

    // Listed again after the file was replaced: the new identity.
    model.StageReset(kFolder, true, &IsTestImage);
    FIC2_CHECK(model.Stage({ File(L"b.png") }, { identity(21, 201) }) == 1);
    FIC2_CHECK(itemIdentity(model) == identity(20, 200));   // displayed until the commit
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(itemIdentity(model) == identity(21, 201));

    // Without identities (or fewer than entries) the rest are unknown.
    model.Reset(kFolder, true, &IsTestImage);
    FIC2_CHECK(model.Merge({ File(L"a.png"), File(L"b.png") }, { identity(10, 100) }) == 2);
    FIC2_CHECK(model.Images()[0].identity == identity(10, 100));
    FIC2_CHECK(!itemIdentity(model).known);
}

FIC2_TEST(ThumbItemModel_ImageExtensionLookup)
{
    // Direct registry lookup, case-insensitive; no extension, unsupported.
    FIC2_CHECK(ImageAwareVfs::IsImageFile(std::wstring(L"photo.PNG")));
    FIC2_CHECK(ImageAwareVfs::IsImageFile(std::wstring(L"texture.dds")));
    FIC2_CHECK(ImageAwareVfs::IsImageFile(std::wstring(L"archive.tar.jpg")));
    FIC2_CHECK(!ImageAwareVfs::IsImageFile(std::wstring(L"notes.txt")));
    FIC2_CHECK(!ImageAwareVfs::IsImageFile(std::wstring(L"noextension")));
    FIC2_CHECK(!ImageAwareVfs::IsImageFile(std::wstring(L"")));
    FIC2_CHECK(!ImageAwareVfs::IsImageFile(std::wstring(L"trailing.")));
}

FIC2_TEST(ThumbItemModel_IndexOfFollowsTheCommittedOrder)
{
    ThumbItemModel model;
    model.Reset(kFolder, true, &IsTestImage);
    (void)model.Merge(Listing());
    FIC2_REQUIRE(model.DisplayCount() == 7);
    for (size_t i = 0; i < model.DisplayCount(); ++i)
    {
        FIC2_CHECK(model.IndexOf(model.At(i).lookupKey) == i);
    }
    // Lookup keys are case-folded; unlisted and unsupported files have none.
    FIC2_CHECK(model.IndexOf(ThumbItemModel::LookupKeyFor(File(L"B.PNG").path)).has_value());
    FIC2_CHECK(!model.IndexOf(ThumbItemModel::LookupKeyFor(File(L"readme.txt").path)));
    FIC2_CHECK(!model.IndexOf(ThumbItemModel::LookupKeyFor(File(L"missing.png").path)));

    // Staged items get an index (and shift later ones) only on the commit.
    const std::wstring bKey = ThumbItemModel::LookupKeyFor(File(L"b.png").path);
    const std::wstring aaKey = ThumbItemModel::LookupKeyFor(File(L"aa.png").path);
    const auto bBefore = model.IndexOf(bKey);
    FIC2_REQUIRE(bBefore.has_value());
    FIC2_CHECK(model.Stage({ File(L"aa.png") }) == 1);
    FIC2_CHECK(!model.IndexOf(aaKey));
    FIC2_CHECK(model.IndexOf(bKey) == bBefore);
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(model.IndexOf(aaKey) == *bBefore);
    FIC2_CHECK(model.IndexOf(bKey) == *bBefore + 1);

    // A staged reset keeps the displayed indices until it is committed.
    model.StageReset(kFolder, false, &IsTestImage);
    FIC2_CHECK(model.Stage({ File(L"c.dds") }) == 1);
    FIC2_CHECK(model.IndexOf(bKey) == *bBefore + 1);
    FIC2_CHECK(model.Commit());
    FIC2_CHECK(!model.IndexOf(bKey));
    FIC2_CHECK(model.IndexOf(ThumbItemModel::LookupKeyFor(File(L"c.dds").path)) == 0u);
}

FIC2_TEST(ThumbItemModel_IndexOfSkipsHiddenNavItems)
{
    ThumbItemModel model;
    model.Reset(kFolder, false, &IsTestImage);
    (void)model.Merge(Listing());
    FIC2_REQUIRE(model.DisplayCount() == 3);
    FIC2_CHECK(!model.IndexOf(ThumbItemModel::LookupKeyFor(Dir(L"alpha").path)));
    FIC2_CHECK(!model.IndexOf(ThumbItemModel::LookupKeyFor(kFolder.GetParent())));
    FIC2_CHECK(model.IndexOf(ThumbItemModel::LookupKeyFor(File(L"A.png").path)) == 0u);
}
