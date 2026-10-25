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
