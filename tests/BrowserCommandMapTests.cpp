#include "TestHarness.h"

#include "BrowserCommandMap.h"
#include "Resource.h"

#include <optional>

// Which command a context-menu item or a key runs (the window executes it).

namespace
{
    using A = BrowserCommandAction;

    std::optional<A> Up(UINT key, bool ctrl = false, bool shift = false, bool alt = false)
    {
        return BrowserCommandMap::ForKeyUp(key, ctrl, shift, alt);
    }
}

FIC2_TEST(BrowserCommandMap_ContextMenuItems)
{
    const struct
    {
        UINT id;
        A action;
    } items[] = {
        { IDM_CTX_OPEN_IMAGE, A::OpenImage },
        { IDM_CTX_OPEN_FOLDER, A::OpenFolder },
        { IDM_CTX_OPEN_NEW_IMAGE, A::OpenNewImage },
        { IDM_CTX_CLOSE, A::Close },
        { IDM_CTX_FIT_TO_SCREEN, A::FitToScreen },
        { IDM_CTX_BACKGROUND_COLOR, A::BackgroundColor },
        { IDM_CTX_FOCUSED_BACKGROUND_COLOR, A::FocusedBackgroundColor },
        { IDM_CTX_TOGGLE_DIRECTORIES, A::ToggleDirectories },
        { IDM_CTX_TOGGLE_ALPHA, A::ToggleAlpha },
        { IDM_CTX_TOGGLE_SAMPLING, A::ToggleSampling },
        { IDM_CTX_ROTATE_LEFT, A::RotateLeft },
        { IDM_CTX_ROTATE_RIGHT, A::RotateRight },
        { IDM_CTX_ROTATE_180, A::Rotate180 },
        { IDM_CTX_ROTATE_RESET, A::RotateReset },
        { IDM_CTX_SHOW_IN_EXPLORER, A::ShowInExplorerAtPoint },
        { IDM_CTX_SAVE_SCREENSHOT, A::SaveScreenshot },
        { IDM_CTX_CLEAR_RECENT, A::ClearRecent },
        { IDM_CTX_ABOUT, A::ShowAbout },
    };
    for (const auto& item : items)
    {
        FIC2_CHECK(BrowserCommandMap::ForContextMenu(item.id) == item.action);
    }
    FIC2_CHECK(!BrowserCommandMap::ForContextMenu(0));

    // Registration items exist only when the registration menu is built in.
#if FICTURE2_ENABLE_REGISTRATION_MENU
    FIC2_CHECK(BrowserCommandMap::ForContextMenu(IDM_CTX_REGISTER_ASSOCIATIONS) == A::RegisterAssociations);
    FIC2_CHECK(BrowserCommandMap::ForContextMenu(IDM_CTX_UNREGISTER_ASSOCIATIONS) == A::UnregisterAssociations);
    FIC2_CHECK(BrowserCommandMap::ForContextMenu(IDM_CTX_REGISTER_THUMBNAIL_PROVIDER) == A::RegisterThumbnailProvider);
    FIC2_CHECK(BrowserCommandMap::ForContextMenu(IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER) == A::UnregisterThumbnailProvider);
#else
    FIC2_CHECK(!BrowserCommandMap::ForContextMenu(IDM_CTX_REGISTER_ASSOCIATIONS));
    FIC2_CHECK(!BrowserCommandMap::ForContextMenu(IDM_CTX_UNREGISTER_ASSOCIATIONS));
    FIC2_CHECK(!BrowserCommandMap::ForContextMenu(IDM_CTX_REGISTER_THUMBNAIL_PROVIDER));
    FIC2_CHECK(!BrowserCommandMap::ForContextMenu(IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER));
#endif
}

FIC2_TEST(BrowserCommandMap_SelectionKeysIgnoreModifiers)
{
    const struct
    {
        UINT key;
        A action;
    } keys[] = {
        { VK_RETURN, A::ActivateSelected },
        { VK_HOME, A::SelectFirst },
        { VK_END, A::SelectLast },
        { VK_PRIOR, A::PagePrevious },
        { VK_NEXT, A::PageNext },
        { VK_BACK, A::NavigateUp },
    };
    for (const auto& k : keys)
    {
        FIC2_CHECK(Up(k.key) == k.action);
        FIC2_CHECK(Up(k.key, true, true, true) == k.action);
    }

    FIC2_CHECK(BrowserCommandMap::ForKeyDown(VK_LEFT) == A::SelectPrevious);
    FIC2_CHECK(BrowserCommandMap::ForKeyDown(VK_RIGHT) == A::SelectNext);
    FIC2_CHECK(!BrowserCommandMap::ForKeyDown(VK_HOME));
    FIC2_CHECK(!BrowserCommandMap::ForKeyDown(VK_UP));
    // Left/Right are key-down commands only.
    FIC2_CHECK(!Up(VK_LEFT));
    FIC2_CHECK(!Up(VK_RIGHT));
}

FIC2_TEST(BrowserCommandMap_LifecycleChords)
{
    FIC2_CHECK(Up(VK_F4, true) == A::Close);
    FIC2_CHECK(Up(VK_F4, true, true) == A::Close);
    FIC2_CHECK(Up('O', true) == A::OpenImage);
    FIC2_CHECK(Up('o', true) == A::OpenImage);
    FIC2_CHECK(Up('O', true, true) == A::OpenImageSplitNew);
    // Alt does not change them.
    FIC2_CHECK(Up(VK_F4, true, false, true) == A::Close);
    FIC2_CHECK(Up('O', true, false, true) == A::OpenImage);
    // Without Ctrl, nothing.
    FIC2_CHECK(!Up(VK_F4));
    FIC2_CHECK(!Up('O'));
    FIC2_CHECK(!Up('O', false, true));
}

FIC2_TEST(BrowserCommandMap_MipChordsNeedExactlyCtrl)
{
    FIC2_CHECK(Up(VK_OEM_4, true) == A::MipPrevious);
    FIC2_CHECK(Up(VK_OEM_6, true) == A::MipNext);
    FIC2_CHECK(!Up(VK_OEM_4));
    FIC2_CHECK(!Up(VK_OEM_6));
    FIC2_CHECK(!Up(VK_OEM_4, true, true));
    FIC2_CHECK(!Up(VK_OEM_6, true, false, true));
    FIC2_CHECK(!Up(VK_OEM_4, false, false, true));
}

FIC2_TEST(BrowserCommandMap_AltChordsIgnoreCtrl)
{
    const struct
    {
        UINT key;
        A action;
    } chords[] = {
        { VK_UP, A::NavigateUp },
        { 'N', A::ToggleDirectories },
        { 'A', A::ToggleAlpha },
        { 'X', A::FitToScreen },
        { 'B', A::BackgroundColor },
        { 'Q', A::ToggleSampling },
    };
    for (const auto& c : chords)
    {
        FIC2_CHECK(Up(c.key, false, false, true) == c.action);
        FIC2_CHECK(Up(c.key, true, false, true) == c.action);
        FIC2_CHECK(!Up(c.key));
        FIC2_CHECK(!Up(c.key, false, true, true));
    }
    // Letters in either case.
    FIC2_CHECK(Up('n', false, false, true) == A::ToggleDirectories);
    FIC2_CHECK(Up('q', false, false, true) == A::ToggleSampling);

    // < / > (Shift+comma / Shift+period), with or without Ctrl.
    FIC2_CHECK(Up(VK_OEM_COMMA, false, true) == A::RotateLeft);
    FIC2_CHECK(Up(VK_OEM_PERIOD, false, true) == A::RotateRight);
    FIC2_CHECK(Up(VK_OEM_COMMA, true, true) == A::RotateLeft);
    FIC2_CHECK(!Up(VK_OEM_COMMA));
    FIC2_CHECK(!Up(VK_OEM_PERIOD, false, true, true));
}

FIC2_TEST(BrowserCommandMap_UnboundKeys)
{
    FIC2_CHECK(!Up('Z'));
    FIC2_CHECK(!Up('Z', true, true, true));
    FIC2_CHECK(!Up(VK_ESCAPE));
    FIC2_CHECK(!Up(VK_F5, true));
    FIC2_CHECK(!BrowserCommandMap::ForKeyDown('Z'));
}
