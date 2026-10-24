#include "TestHarness.h"

#include "ImageBrowserPathState.h"
#include "IpcOpenRequest.h"

#include <string>
#include <vector>

// Selected / requested / displayed path transitions of one browser pane, and
// the IPC open-name snapshot built from them. The browser forwards selection
// and main-image presentation events into ImageBrowserPathState exactly as
// these tests do.

namespace
{
    const std::wstring kA = L"C:\\pics\\a.dds";
    const std::wstring kB = L"C:\\pics\\b.dds";
    const std::wstring kC = L"C:\\pics\\c.dds";
    const std::wstring kFolder = L"C:\\pics\\sub";

    // The main image reports normalized paths; the browser requests display paths.
    std::wstring AsPresented(const std::wstring& path)
    {
        return CommonUtil::NormalizePath(path);
    }

    // Mirrors ImageBrowserMainImage's presentation reporting: SetLoadedFilePath
    // notifies only when the presented path changes, and a GPU cache hit
    // presents synchronously inside SetSourceFile.
    struct FakeMainImage
    {
        ImageBrowserPathState& paths;
        std::vector<std::wstring> cached {};
        std::wstring requested {};
        std::wstring loaded {};

        void SetLoadedFilePath(const std::wstring& path)
        {
            if (path == loaded)
            {
                return;
            }
            loaded = path;
            if (loaded.empty())
            {
                paths.OnPresentationCleared();
            }
            else
            {
                paths.OnPresented(loaded);
            }
        }

        void SetSourceFile(const std::wstring& path)
        {
            requested = CommonUtil::NormalizePath(path);
            for (const auto& c : cached)
            {
                if (c == requested)
                {
                    SetLoadedFilePath(requested);  // synchronous cache hit
                }
            }
        }

        // An asynchronous decode of the current source completes.
        void CompleteLoad()
        {
            SetLoadedFilePath(requested);
            cached.push_back(requested);
        }
    };

    // ImageBrowserImpl::ApplyMainFromIndex
    void SelectAndLoad(ImageBrowserPathState& paths, FakeMainImage& main, const std::wstring& path)
    {
        paths.RequestImage(path, [&]() { main.SetSourceFile(path); });
    }

    // IPC snapshot from several panes, as Ficture2Backplate::RefreshIpcOpenSnapshot builds it.
    void Publish(IpcOpenQueue& queue, const std::vector<const ImageBrowserPathState*>& panes)
    {
        std::vector<std::wstring> requested;
        for (const auto* pane : panes)
        {
            requested.push_back(pane->requested);
        }
        queue.PublishOpenNames(requested);
    }
}

FIC2_TEST(PathState_NewSelectionIsRequestedWhilePreviousStaysDisplayed)
{
    ImageBrowserPathState paths;
    paths.SelectImage(kA);
    FIC2_CHECK(paths.OnPresented(AsPresented(kA)));
    FIC2_CHECK(paths.displayed == AsPresented(kA));

    paths.SelectImage(kB);
    FIC2_CHECK(paths.selected == kB);
    FIC2_CHECK(paths.requested == kB);
    FIC2_CHECK(paths.displayed == AsPresented(kA));  // a stays on screen while b loads
    FIC2_CHECK(paths.RequestedFileNameLower() == L"b.dds");
    FIC2_CHECK(paths.RequestedFileName() == L"b.dds");

    FIC2_CHECK(paths.OnPresented(AsPresented(kB)));
    FIC2_CHECK(paths.displayed == AsPresented(kB));
}

FIC2_TEST(PathState_StaleOrFailedLoadNeverBecomesDisplayed)
{
    ImageBrowserPathState paths;
    paths.SelectImage(kA);
    FIC2_REQUIRE(paths.OnPresented(AsPresented(kA)));

    // b fails: no presentation is reported, a remains displayed.
    paths.SelectImage(kB);
    FIC2_CHECK(paths.displayed == AsPresented(kA));

    // c is requested; a late presentation of the superseded b is rejected.
    paths.SelectImage(kC);
    FIC2_CHECK(!paths.OnPresented(AsPresented(kB)));
    FIC2_CHECK(paths.displayed == AsPresented(kA));
    FIC2_CHECK(!paths.OnPresented(L""));

    // Case and separator differences between display and presented paths match.
    FIC2_CHECK(paths.OnPresented(L"c:/PICS/C.DDS"));
}

FIC2_TEST(PathState_NonImageSelectionHasNoRequestedImage)
{
    ImageBrowserPathState paths;
    paths.SelectImage(kA);
    FIC2_REQUIRE(paths.OnPresented(AsPresented(kA)));

    paths.SelectNonImage(kFolder);
    FIC2_CHECK(paths.selected == kFolder);
    FIC2_CHECK(!paths.HasRequest());
    FIC2_CHECK(paths.RequestedFileNameLower().empty());
    FIC2_CHECK(paths.RequestedFileName().empty());
    // Nothing can be presented without a request; the browser clears the image.
    FIC2_CHECK(!paths.OnPresented(AsPresented(kA)));
    paths.OnPresentationCleared();
    FIC2_CHECK(paths.displayed.empty());

    paths.ClearSelection();
    FIC2_CHECK(paths.selected.empty() && paths.requested.empty());
}

FIC2_TEST(PathState_RapidSelectionKeepsOnlyLatestRequest)
{
    ImageBrowserPathState paths;
    std::vector<std::wstring> sequence;
    for (int i = 0; i < 200; ++i)
    {
        sequence.push_back(L"C:\\pics\\img" + std::to_wstring(i) + L".png");
    }
    paths.SelectImage(sequence.front());
    FIC2_REQUIRE(paths.OnPresented(AsPresented(sequence.front())));

    for (size_t i = 1; i < sequence.size(); ++i)
    {
        paths.SelectImage(sequence[i]);
        // Presentations of earlier selections arrive late and are ignored.
        FIC2_CHECK(!paths.OnPresented(AsPresented(sequence[i - 1])));
        FIC2_CHECK(paths.requested == sequence[i]);
    }
    FIC2_CHECK(paths.displayed == AsPresented(sequence.front()));
    FIC2_CHECK(paths.OnPresented(AsPresented(sequence.back())));
    FIC2_CHECK(paths.displayed == AsPresented(sequence.back()));
}

FIC2_TEST(PathState_SynchronousCacheHitPresentationIsDisplayed)
{
    // Load A, load B, then reselect A from the GPU cache: A is presented inside
    // SetSourceFile, before ApplyMainFromIndex returns. It must become the
    // displayed path; the main image does not report the same path again, so a
    // rejected report would leave B "displayed" for good.
    ImageBrowserPathState paths;
    FakeMainImage main { paths };

    SelectAndLoad(paths, main, kA);
    FIC2_CHECK(paths.displayed.empty());
    main.CompleteLoad();
    FIC2_CHECK(paths.displayed == AsPresented(kA));

    SelectAndLoad(paths, main, kB);
    main.CompleteLoad();
    FIC2_CHECK(paths.displayed == AsPresented(kB));

    SelectAndLoad(paths, main, kA);  // cache hit: presents synchronously
    FIC2_CHECK(main.loaded == AsPresented(kA));
    FIC2_CHECK(paths.requested == kA);
    FIC2_CHECK(paths.displayed == AsPresented(kA));
}

FIC2_TEST(PathState_IpcSnapshotFollowsRequestedNotDisplayed)
{
    // Pane 1 shows a.dds and the user just selected b.dds (still loading);
    // pane 2 shows c.dds. A second instance opening another folder's b.dds must
    // be accepted into compare, and a.dds must no longer match.
    ImageBrowserPathState pane1;
    ImageBrowserPathState pane2;
    pane1.SelectImage(kA);
    FIC2_REQUIRE(pane1.OnPresented(AsPresented(kA)));
    pane2.SelectImage(kC);
    FIC2_REQUIRE(pane2.OnPresented(AsPresented(kC)));
    pane1.SelectImage(kB);

    IpcOpenQueue queue;
    queue.MarkUiReady();
    Publish(queue, { &pane1, &pane2 });
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        FIC2_CHECK(queue.loadedCount == 2);
        FIC2_REQUIRE(queue.openNamesLower.size() == 2);
        FIC2_CHECK(queue.openNamesLower[0] == L"b.dds");
        FIC2_CHECK(queue.openNamesLower[1] == L"c.dds");
    }
    FIC2_CHECK(!queue.TryEnqueue(L"D:\\other\\a.dds", 8));
    FIC2_CHECK(queue.TryEnqueue(L"D:\\other\\B.DDS", 8));

    // A pane on a folder item has no requested image and is not an open document.
    pane2.SelectNonImage(kFolder);
    Publish(queue, { &pane1, &pane2 });
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        FIC2_CHECK(queue.loadedCount == 1);
        FIC2_REQUIRE(queue.openNamesLower.size() == 1);
        FIC2_CHECK(queue.openNamesLower[0] == L"b.dds");
    }
    FIC2_CHECK(!queue.TryEnqueue(L"D:\\other\\c.dds", 8));
}

FIC2_TEST(PathState_PublishSkipsEmptyRequests)
{
    IpcOpenQueue queue;
    queue.PublishOpenNames({ L"", L"C:\\x\\One.PNG", L"" });
    std::lock_guard<std::mutex> lock(queue.mutex);
    FIC2_CHECK(queue.loadedCount == 1);
    FIC2_REQUIRE(queue.openNamesLower.size() == 1);
    FIC2_CHECK(queue.openNamesLower[0] == L"one.png");
}
