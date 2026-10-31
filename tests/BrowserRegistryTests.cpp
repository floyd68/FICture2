#include "TestHarness.h"

#include "BrowserRegistry.h"

#include <cstdint>
#include <vector>

// The browser registry without a window: null and duplicate registration,
// unregistering, registration order (the first browser is the root host),
// the count and snapshot independence. The pointers are never dereferenced.

namespace
{
    FD2D::Wnd* Fake(uintptr_t n)
    {
        return reinterpret_cast<FD2D::Wnd*>(n * 0x100);
    }
}

FIC2_TEST(BrowserRegistry_RegisterIgnoresNullAndDuplicates)
{
    BrowserRegistry r;
    FIC2_CHECK(r.ImageBrowserCount() == 0);
    FIC2_CHECK(r.ImageBrowsersSnapshot().empty());

    r.RegisterImageBrowser(nullptr);
    FIC2_CHECK(r.ImageBrowserCount() == 0);

    r.RegisterImageBrowser(Fake(1));
    r.RegisterImageBrowser(Fake(1));
    FIC2_CHECK(r.ImageBrowserCount() == 1);
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(1) }));
}

FIC2_TEST(BrowserRegistry_Unregister)
{
    BrowserRegistry r;
    r.RegisterImageBrowser(Fake(1));
    r.RegisterImageBrowser(Fake(2));

    r.UnregisterImageBrowser(Fake(9));     // unknown: no-op
    r.UnregisterImageBrowser(nullptr);
    FIC2_CHECK(r.ImageBrowserCount() == 2);

    r.UnregisterImageBrowser(Fake(1));
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(2) }));
    r.UnregisterImageBrowser(Fake(1));     // already gone
    FIC2_CHECK(r.ImageBrowserCount() == 1);

    // Registering again after removal appends at the end.
    r.RegisterImageBrowser(Fake(1));
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(2), Fake(1) }));
}

FIC2_TEST(BrowserRegistry_KeepsRegistrationOrder)
{
    BrowserRegistry r;
    for (uintptr_t i = 1; i <= 4; ++i)
    {
        r.RegisterImageBrowser(Fake(i));
    }
    FIC2_CHECK(r.ImageBrowserCount() == 4);
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(1), Fake(2), Fake(3), Fake(4) }));

    // Removing a middle entry keeps the others in order; the first (the root
    // host) stays first.
    r.UnregisterImageBrowser(Fake(3));
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(1), Fake(2), Fake(4) }));

    // Removing the first promotes the next one.
    r.UnregisterImageBrowser(Fake(1));
    FIC2_CHECK(r.ImageBrowsersSnapshot().front() == Fake(2));
}

FIC2_TEST(BrowserRegistry_SnapshotIsACopy)
{
    BrowserRegistry r;
    r.RegisterImageBrowser(Fake(1));
    r.RegisterImageBrowser(Fake(2));

    const std::vector<FD2D::Wnd*> snapshot = r.ImageBrowsersSnapshot();
    r.UnregisterImageBrowser(Fake(1));
    r.RegisterImageBrowser(Fake(3));

    FIC2_CHECK(snapshot == std::vector<FD2D::Wnd*>({ Fake(1), Fake(2) }));
    FIC2_CHECK(r.ImageBrowsersSnapshot() == std::vector<FD2D::Wnd*>({ Fake(2), Fake(3) }));
}
