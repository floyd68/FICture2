#include "ImageBrowserContextMenu.h"

#include "Resource.h"
#include "AppSetup.h"
#include "RecentFiles.h"
#include "ImageBrowserSplitCoordinator.h"

#include <algorithm>

namespace ImageBrowserContextMenu
{
    void Configure(HMENU hPopup, const ConfigurePayload& payload)
    {
        EnableMenuItem(
            hPopup,
            IDM_CTX_OPEN_NEW_IMAGE,
            MF_BYCOMMAND | ((payload.viewerCount < static_cast<int>(ImageBrowserSplitCoordinator::kMaxViewers))
                ? MF_ENABLED
                : MF_GRAYED));

        EnableMenuItem(
            hPopup,
            IDM_CTX_CLOSE,
            MF_BYCOMMAND | (payload.canClose ? MF_ENABLED : MF_GRAYED));

        ModifyMenuW(
            hPopup,
            IDM_CTX_TOGGLE_DIRECTORIES,
            MF_BYCOMMAND | MF_STRING,
            IDM_CTX_TOGGLE_DIRECTORIES,
            payload.showNavItems ? L"Hide Directories\tAlt+N" : L"Show Directories\tAlt+N");

        ModifyMenuW(
            hPopup,
            IDM_CTX_TOGGLE_ALPHA,
            MF_BYCOMMAND | MF_STRING,
            IDM_CTX_TOGGLE_ALPHA,
            payload.showAlpha ? L"Hide Alpha\tAlt+A" : L"Show Alpha\tAlt+A");

        ModifyMenuW(
            hPopup,
            IDM_CTX_TOGGLE_SAMPLING,
            MF_BYCOMMAND | MF_STRING,
            IDM_CTX_TOGGLE_SAMPLING,
            (std::wstring(L"Sampling: ") + payload.samplingLabel + L"\tAlt+Q").c_str());

        // Open Recent submenu (owned by hPopup once attached).
        HMENU recentMenu = CreatePopupMenu();
        if (recentMenu != nullptr)
        {
            if (!payload.recentFiles.empty())
            {
                const std::size_t count = (std::min)(
                    payload.recentFiles.size(),
                    static_cast<std::size_t>(IDM_CTX_RECENT_LAST - IDM_CTX_RECENT_BASE + 1));
                for (std::size_t i = 0; i < count; ++i)
                {
                    AppendMenuW(
                        recentMenu,
                        MF_STRING,
                        IDM_CTX_RECENT_BASE + static_cast<UINT>(i),
                        RecentFiles::MenuLabel(payload.recentFiles[i]).c_str());
                }
                AppendMenuW(recentMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(recentMenu, MF_STRING, IDM_CTX_CLEAR_RECENT, L"&Clear Recent Files");
            }

            const UINT recentFlags = MF_POPUP | (payload.recentFiles.empty() ? MF_GRAYED : MF_ENABLED);
            InsertMenuW(
                hPopup,
                IDM_CTX_OPEN_NEW_IMAGE,
                MF_BYCOMMAND | recentFlags,
                reinterpret_cast<UINT_PTR>(recentMenu),
                L"Open &Recent");
        }

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            hPopup,
            MF_STRING | (payload.hasExplorerTarget ? MF_ENABLED : MF_GRAYED),
            IDM_CTX_SHOW_IN_EXPLORER,
            L"Show in &Explorer");
        AppendMenuW(
            hPopup,
            MF_STRING | (payload.canSaveScreenshot ? MF_ENABLED : MF_GRAYED),
            IDM_CTX_SAVE_SCREENSHOT,
            L"Save Pane &Screenshot...");

#if FICTURE2_ENABLE_REGISTRATION_MENU
        ModifyMenuW(
            hPopup,
            IDM_CTX_REGISTER_ASSOCIATIONS,
            MF_BYCOMMAND | MF_STRING,
            payload.associationsRegistered
                ? IDM_CTX_UNREGISTER_ASSOCIATIONS
                : IDM_CTX_REGISTER_ASSOCIATIONS,
            payload.associationsRegistered
                ? L"Unregister &File Associations..."
                : L"Register &File Associations...");

        ModifyMenuW(
            hPopup,
            IDM_CTX_REGISTER_THUMBNAIL_PROVIDER,
            MF_BYCOMMAND | MF_STRING,
            payload.thumbRegistered ? IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER : IDM_CTX_REGISTER_THUMBNAIL_PROVIDER,
            payload.thumbRegistered
                ? L"Unregister &Thumbnail Provider (Admin)..."
                : L"Register &Thumbnail Provider (Admin)...");
#else
        DeleteMenu(hPopup, IDM_CTX_REGISTER_ASSOCIATIONS, MF_BYCOMMAND);
        DeleteMenu(hPopup, IDM_CTX_UNREGISTER_ASSOCIATIONS, MF_BYCOMMAND);
        DeleteMenu(hPopup, IDM_CTX_REGISTER_THUMBNAIL_PROVIDER, MF_BYCOMMAND);
        DeleteMenu(hPopup, IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER, MF_BYCOMMAND);
#endif

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hPopup, MF_STRING, IDM_CTX_ABOUT, L"&About...");
    }

    UINT TrackAndReturnCommand(HMENU hPopup, HWND hwnd, const POINT& clientPt)
    {
        POINT ptScreen = clientPt;
        ClientToScreen(hwnd, &ptScreen);

        SetForegroundWindow(hwnd);
        const UINT cmd = TrackPopupMenuEx(
            hPopup,
            TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            ptScreen.x,
            ptScreen.y,
            hwnd,
            nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        return cmd;
    }
}
