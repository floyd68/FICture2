#include "BrowserCommandMap.h"

#include "Resource.h"

namespace
{
    struct CommandMapEntry
    {
        UINT id { 0 };
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
    };

    struct KeyChordMapEntry
    {
        UINT key { 0 };
        bool ctrl { false };
        bool shift { false };
        bool alt { false };
        BrowserCommandAction action { BrowserCommandAction::OpenImage };
    };

    // Context menu: file/viewer lifecycle commands.
    const CommandMapEntry kContextLifecycleMap[] =
    {
        { IDM_CTX_OPEN_IMAGE, BrowserCommandAction::OpenImage },
        { IDM_CTX_OPEN_FOLDER, BrowserCommandAction::OpenFolder },
        { IDM_CTX_OPEN_NEW_IMAGE, BrowserCommandAction::OpenNewImage },
        { IDM_CTX_CLOSE, BrowserCommandAction::Close },
    };

    // Context menu: view/appearance commands.
    const CommandMapEntry kContextViewAppearanceMap[] =
    {
        { IDM_CTX_FIT_TO_SCREEN, BrowserCommandAction::FitToScreen },
        { IDM_CTX_BACKGROUND_COLOR, BrowserCommandAction::BackgroundColor },
        { IDM_CTX_FOCUSED_BACKGROUND_COLOR, BrowserCommandAction::FocusedBackgroundColor },
        { IDM_CTX_TOGGLE_DIRECTORIES, BrowserCommandAction::ToggleDirectories },
        { IDM_CTX_TOGGLE_ALPHA, BrowserCommandAction::ToggleAlpha },
        { IDM_CTX_TOGGLE_SAMPLING, BrowserCommandAction::ToggleSampling },
        { IDM_CTX_ROTATE_LEFT,  BrowserCommandAction::RotateLeft  },
        { IDM_CTX_ROTATE_RIGHT, BrowserCommandAction::RotateRight },
        { IDM_CTX_ROTATE_180,   BrowserCommandAction::Rotate180   },
        { IDM_CTX_ROTATE_RESET, BrowserCommandAction::RotateReset },
    };

    // Context menu: integration/system commands.
    const CommandMapEntry kContextIntegrationMap[] =
    {
        { IDM_CTX_SHOW_IN_EXPLORER, BrowserCommandAction::ShowInExplorerAtPoint },
        { IDM_CTX_SAVE_SCREENSHOT, BrowserCommandAction::SaveScreenshot },
        { IDM_CTX_CLEAR_RECENT, BrowserCommandAction::ClearRecent },
        { IDM_CTX_ABOUT, BrowserCommandAction::ShowAbout },
    };

#if FICTURE2_ENABLE_REGISTRATION_MENU
    const CommandMapEntry kContextRegistrationMap[] =
    {
        { IDM_CTX_REGISTER_ASSOCIATIONS, BrowserCommandAction::RegisterAssociations },
        { IDM_CTX_UNREGISTER_ASSOCIATIONS, BrowserCommandAction::UnregisterAssociations },
        { IDM_CTX_REGISTER_THUMBNAIL_PROVIDER, BrowserCommandAction::RegisterThumbnailProvider },
        { IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER, BrowserCommandAction::UnregisterThumbnailProvider },
    };
#endif

    // KeyUp: selection/navigation commands without modifier chords.
    const CommandMapEntry kKeySelectionNavigationMap[] =
    {
        { VK_RETURN, BrowserCommandAction::ActivateSelected },
        { VK_HOME, BrowserCommandAction::SelectFirst },
        { VK_END, BrowserCommandAction::SelectLast },
        { VK_PRIOR, BrowserCommandAction::PagePrevious },
        { VK_NEXT, BrowserCommandAction::PageNext },
        { VK_BACK, BrowserCommandAction::NavigateUp },
    };

    // KeyDown: immediate left/right selection movement.
    const CommandMapEntry kKeyDownSelectionNavigationMap[] =
    {
        { VK_LEFT, BrowserCommandAction::SelectPrevious },
        { VK_RIGHT, BrowserCommandAction::SelectNext },
    };

    // KeyUp: lifecycle modifier chords (Ctrl/Ctrl+Shift).
    const KeyChordMapEntry kKeyLifecycleChordMap[] =
    {
        { VK_F4, true, false, false, BrowserCommandAction::Close },
        { VK_F4, true, true, false, BrowserCommandAction::Close },
        { 'O', true, false, false, BrowserCommandAction::OpenImage },
        { 'O', true, true, false, BrowserCommandAction::OpenImageSplitNew },
    };

    // KeyUp: Alt+* appearance/navigation chords.
    const KeyChordMapEntry kKeyViewAppearanceChordMap[] =
    {
        { VK_UP, false, false, true, BrowserCommandAction::NavigateUp },
        { 'N', false, false, true, BrowserCommandAction::ToggleDirectories },
        { 'A', false, false, true, BrowserCommandAction::ToggleAlpha },
        { 'X', false, false, true, BrowserCommandAction::FitToScreen },
        { 'B', false, false, true, BrowserCommandAction::BackgroundColor },
        { 'Q', false, false, true, BrowserCommandAction::ToggleSampling },
        // < / > : rotate left / right (Shift+comma / Shift+period)
        { VK_OEM_COMMA,  false, true, false, BrowserCommandAction::RotateLeft  },
        { VK_OEM_PERIOD, false, true, false, BrowserCommandAction::RotateRight },
    };

    // KeyUp: Ctrl+[ / Ctrl+] : previous / next DDS mip level. Matched with the
    // actual modifiers (the view chords above ignore Ctrl).
    const KeyChordMapEntry kKeyMipChordMap[] =
    {
        { VK_OEM_4, true, false, false, BrowserCommandAction::MipPrevious },
        { VK_OEM_6, true, false, false, BrowserCommandAction::MipNext },
    };

    template <size_t N>
    std::optional<BrowserCommandAction> Find(const CommandMapEntry (&entries)[N], UINT id)
    {
        for (const auto& entry : entries)
        {
            if (entry.id == id)
            {
                return entry.action;
            }
        }
        return std::nullopt;
    }

    template <size_t N>
    std::optional<BrowserCommandAction> Find(const KeyChordMapEntry (&entries)[N], UINT key, bool ctrl, bool shift, bool alt)
    {
        for (const auto& entry : entries)
        {
            if (entry.key == key && entry.ctrl == ctrl && entry.shift == shift && entry.alt == alt)
            {
                return entry.action;
            }
        }
        return std::nullopt;
    }
}

namespace BrowserCommandMap
{
    std::optional<BrowserCommandAction> ForContextMenu(UINT id)
    {
        if (const auto action = Find(kContextLifecycleMap, id))
        {
            return action;
        }
        if (const auto action = Find(kContextViewAppearanceMap, id))
        {
            return action;
        }
        if (const auto action = Find(kContextIntegrationMap, id))
        {
            return action;
        }
#if FICTURE2_ENABLE_REGISTRATION_MENU
        if (const auto action = Find(kContextRegistrationMap, id))
        {
            return action;
        }
#endif
        return std::nullopt;
    }

    std::optional<BrowserCommandAction> ForKeyUp(UINT key, bool ctrl, bool shift, bool alt)
    {
        const UINT normalizedKey = (key >= 'a' && key <= 'z') ? (key - 'a' + 'A') : key;

        if (const auto action = Find(kKeySelectionNavigationMap, key))
        {
            return action;
        }
        const UINT keyForLifecycleMatch = (normalizedKey == 'O') ? normalizedKey : key;
        if (const auto action = Find(kKeyLifecycleChordMap, keyForLifecycleMatch, ctrl, shift, false))
        {
            return action;
        }
        if (const auto action = Find(kKeyMipChordMap, key, ctrl, shift, alt))
        {
            return action;
        }
        // Actual Shift, so Shift+<key> bindings (< and >) can match.
        return Find(kKeyViewAppearanceChordMap, normalizedKey, false, shift, alt);
    }

    std::optional<BrowserCommandAction> ForKeyDown(UINT key)
    {
        return Find(kKeyDownSelectionNavigationMap, key);
    }
}
