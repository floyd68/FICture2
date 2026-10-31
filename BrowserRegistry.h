#pragma once

#include <cstddef>
#include <vector>

namespace FD2D
{
    class Wnd;
}

// The image browsers open in one window, in registration order: the first is
// the root horizontal host. Browsers register when attached and unregister
// when detached or destroyed. The backplate owns the registry through a
// shared_ptr; browsers hold a weak_ptr, so one that outlives the backplate
// does not touch a destroyed registry. Pointers are not owned.
class BrowserRegistry
{
public:
    // Ignores null and an already registered browser.
    void RegisterImageBrowser(FD2D::Wnd* browser);
    // Ignores a browser that is not registered.
    void UnregisterImageBrowser(FD2D::Wnd* browser);

    // A copy: registering or unregistering while iterating it is safe.
    std::vector<FD2D::Wnd*> ImageBrowsersSnapshot() const { return m_imageBrowsers; }
    size_t ImageBrowserCount() const { return m_imageBrowsers.size(); }

private:
    std::vector<FD2D::Wnd*> m_imageBrowsers {};
};
