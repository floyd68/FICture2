#include "TestHarness.h"

#include "IpcOpenRequest.h"

// Covers the queue-or-decline gate that IPC worker threads evaluate for
// single-instance "open this file" forwarding.

FIC2_TEST(IpcOpenQueue_DeclinesWhileUiNotReady)
{
    IpcOpenQueue queue;
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\a\\x.dds", 8));
    FIC2_CHECK(queue.pending.empty());
}

FIC2_TEST(IpcOpenQueue_EmptyReadyViewerAcceptsAnyName)
{
    IpcOpenQueue queue;
    queue.MarkUiReady();
    FIC2_CHECK(queue.TryEnqueue(L"C:\\a\\x.dds", 8));
    FIC2_REQUIRE(queue.pending.size() == 1);
    FIC2_CHECK(queue.pending.front() == L"C:\\a\\x.dds");
}

FIC2_TEST(IpcOpenQueue_MatchesOpenNameCaseInsensitively)
{
    IpcOpenQueue queue;
    queue.MarkUiReady();
    queue.SeedExpected({ L"C:\\left\\Texture.DDS" });

    FIC2_CHECK(queue.TryEnqueue(L"D:\\right\\texture.dds", 8));
    FIC2_CHECK(!queue.TryEnqueue(L"D:\\right\\other.dds", 8));
    FIC2_CHECK(queue.pending.size() == 1);
}

FIC2_TEST(IpcOpenQueue_MatchesAlreadyQueuedName)
{
    IpcOpenQueue queue;
    queue.SeedExpected({ L"C:\\a\\x.dds" });
    FIC2_REQUIRE(queue.TryEnqueue(L"D:\\b\\x.dds", 8));

    // Matches the open name and the queued path alike.
    FIC2_CHECK(queue.TryEnqueue(L"E:\\c\\X.dds", 8));
    FIC2_CHECK(queue.pending.size() == 2);
}

FIC2_TEST(IpcOpenQueue_NonEmptyQueueRejectsUnmatchedNameEvenWhenReady)
{
    // With nothing open but a queued path, only that queued name matches.
    IpcOpenQueue queue;
    queue.MarkUiReady();
    FIC2_REQUIRE(queue.TryEnqueue(L"C:\\a\\x.dds", 8));
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\a\\y.dds", 8));
}

FIC2_TEST(IpcOpenQueue_RespectsPaneCapacity)
{
    IpcOpenQueue queue;
    queue.SeedExpected({ L"C:\\a\\x.dds", L"C:\\b\\x.dds" });
    FIC2_CHECK(queue.loadedCount == 2);

    FIC2_CHECK(queue.TryEnqueue(L"C:\\c\\x.dds", 3));
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\d\\x.dds", 3));
    FIC2_CHECK(queue.pending.size() == 1);
}

FIC2_TEST(IpcOpenQueue_DeclinesAfterShutdown)
{
    IpcOpenQueue queue;
    queue.MarkUiReady();
    queue.MarkShuttingDown();
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\a\\x.dds", 8));
}

FIC2_TEST(IpcOpenQueue_DeclinesPathWithoutFileName)
{
    IpcOpenQueue queue;
    queue.MarkUiReady();
    FIC2_CHECK(IpcOpenQueue::FileNameLower(L"C:\\a\\").empty());
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\a\\", 8));
}

FIC2_TEST(IpcOpenQueue_SeedReplacesSnapshot)
{
    IpcOpenQueue queue;
    queue.SeedExpected({ L"C:\\a\\x.dds" });
    queue.SeedExpected({ L"C:\\a\\y.dds" });
    FIC2_CHECK(!queue.TryEnqueue(L"C:\\b\\x.dds", 8));
    FIC2_CHECK(queue.TryEnqueue(L"C:\\b\\y.dds", 8));
}
