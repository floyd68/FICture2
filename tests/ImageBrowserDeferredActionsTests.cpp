#include "TestHarness.h"

#include "ImageBrowserDeferredActions.h"

#include <string>
#include <vector>

// The per-browser deferred-action queue: actions queued before the first one
// runs are kept and run oldest first with their own payloads, an action queued
// while another runs goes behind the ones already waiting, and a full queue
// drops the new action while keeping the queued ones.

namespace
{
    using ImageBrowserDeferredActions::DeferredAction;
    using ImageBrowserDeferredActions::DeferredQueue;

    // Stand-ins for ImageBrowserImpl's DeferredActionKind values.
    constexpr int kActivateSelected = 1;
    constexpr int kNavigateToFolder = 2;
    constexpr int kInsertAfterName = 3;

    struct Ran
    {
        int kind { 0 };
        Floar::VirtualPath path {};
        std::wstring text {};
    };

    // Dispatches like ImageBrowserImpl::RunDeferredAction, by payload shape.
    bool RunNext(DeferredQueue& queue, std::vector<Ran>& ran)
    {
        const auto action = queue.Pop();
        if (!action)
        {
            return false;
        }
        return ImageBrowserDeferredActions::Dispatch(
            *action,
            [&](int kind)
            {
                if (kind != kActivateSelected)
                {
                    return false;
                }
                ran.push_back({ kind, {}, {} });
                return true;
            },
            [&](int kind, const Floar::VirtualPath& path)
            {
                if (kind != kNavigateToFolder)
                {
                    return false;
                }
                ran.push_back({ kind, path, {} });
                return true;
            },
            [&](int kind, const Floar::VirtualPath& path, const std::wstring& text)
            {
                if (kind != kInsertAfterName)
                {
                    return false;
                }
                ran.push_back({ kind, path, text });
                return true;
            });
    }
}

FIC2_TEST(DeferredActions_TwoQueuedActionsRunInFifoOrder)
{
    // Click a folder tile, then press Enter before the first message ran.
    DeferredQueue queue;
    const Floar::VirtualPath folder(std::filesystem::path(L"C:\\pictures\\trip"));
    FIC2_CHECK(queue.Push({ kNavigateToFolder, folder, {} }));
    FIC2_CHECK(queue.Push({ kActivateSelected, {}, {} }));
    FIC2_CHECK(queue.Size() == 2);

    std::vector<Ran> ran;
    FIC2_CHECK(RunNext(queue, ran));
    FIC2_CHECK(RunNext(queue, ran));
    FIC2_CHECK(!RunNext(queue, ran));   // one action per message; nothing left

    FIC2_REQUIRE(ran.size() == 2);
    FIC2_CHECK(ran[0].kind == kNavigateToFolder);
    FIC2_CHECK(ran[0].path == folder);
    FIC2_CHECK(ran[1].kind == kActivateSelected);
    FIC2_CHECK(queue.Empty());
}

FIC2_TEST(DeferredActions_SameKindActionsKeepTheirOwnPayloads)
{
    // Two files dropped in quick succession, each inserting a pane.
    DeferredQueue queue;
    const Floar::VirtualPath first(std::filesystem::path(L"C:\\a\\one.png"));
    const Floar::VirtualPath second(std::filesystem::path(L"C:\\b\\two.png"));
    FIC2_CHECK(queue.Push({ kInsertAfterName, first, L"pane-1" }));
    FIC2_CHECK(queue.Push({ kInsertAfterName, second, L"pane-2" }));

    std::vector<Ran> ran;
    while (RunNext(queue, ran))
    {
    }
    FIC2_REQUIRE(ran.size() == 2);
    FIC2_CHECK(ran[0].path == first);
    FIC2_CHECK(ran[0].text == L"pane-1");
    FIC2_CHECK(ran[1].path == second);
    FIC2_CHECK(ran[1].text == L"pane-2");
}

FIC2_TEST(DeferredActions_ActionQueuedWhileOneRunsGoesLast)
{
    DeferredQueue queue;
    const Floar::VirtualPath folder(std::filesystem::path(L"C:\\pictures"));
    FIC2_CHECK(queue.Push({ kNavigateToFolder, folder, {} }));
    FIC2_CHECK(queue.Push({ kActivateSelected, {}, {} }));

    // The first action queues another one while it runs.
    std::vector<int> order;
    const auto first = queue.Pop();
    FIC2_REQUIRE(first.has_value());
    order.push_back(first->kind);
    FIC2_CHECK(queue.Push({ kInsertAfterName, folder, L"pane-1" }));

    while (const auto next = queue.Pop())
    {
        order.push_back(next->kind);
    }
    FIC2_CHECK((order == std::vector<int> { kNavigateToFolder, kActivateSelected, kInsertAfterName }));
}

FIC2_TEST(DeferredActions_FullQueueDropsTheNewActionAndKeepsTheQueuedOnes)
{
    DeferredQueue queue;
    for (std::size_t i = 0; i < DeferredQueue::kCapacity; ++i)
    {
        FIC2_REQUIRE(queue.Push({ kInsertAfterName, {}, std::to_wstring(i) }));
    }
    FIC2_CHECK(!queue.Push({ kActivateSelected, {}, {} }));
    FIC2_CHECK(queue.Size() == DeferredQueue::kCapacity);

    std::vector<Ran> ran;
    while (RunNext(queue, ran))
    {
    }
    FIC2_REQUIRE(ran.size() == DeferredQueue::kCapacity);
    for (std::size_t i = 0; i < ran.size(); ++i)
    {
        FIC2_CHECK(ran[i].kind == kInsertAfterName);
        FIC2_CHECK(ran[i].text == std::to_wstring(i));
    }

    // Draining frees room again.
    FIC2_CHECK(queue.Push({ kActivateSelected, {}, {} }));
    FIC2_CHECK(RunNext(queue, ran));
    FIC2_CHECK(ran.back().kind == kActivateSelected);
}

FIC2_TEST(DeferredActions_EmptyQueueRunsNothing)
{
    DeferredQueue queue;
    std::vector<Ran> ran;
    FIC2_CHECK(!queue.Pop().has_value());
    FIC2_CHECK(!RunNext(queue, ran));
    FIC2_CHECK(ran.empty());
}
