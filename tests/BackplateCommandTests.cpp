#include "TestHarness.h"

#include "Ficture2Backplate.h"
#include "FD2D/Wnd.h"
#include "ImageBrowser.h"
#include "Resource.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// The window's key and context-menu command dispatch: which browser command a
// key (with its modifiers) or a menu item runs. Commands that open dialogs
// (open, colour pickers, about) are not driven here.

namespace
{
    // An image browser that records the commands it is asked to run.
    class RecordingBrowser final : public FD2D::Wnd, public IImageBrowserOps
    {
    public:
        RecordingBrowser() : FD2D::Wnd(L"recording-browser") {}

        std::vector<std::string> commands {};

        std::string Take()
        {
            std::string all {};
            for (const auto& command : commands)
            {
                all += (all.empty() ? "" : ",") + command;
            }
            commands.clear();
            return all;
        }

        void BrowserCmdClose() override { commands.push_back("Close"); }
        void BrowserCmdActivateSelected() override { commands.push_back("ActivateSelected"); }
        void BrowserCmdSelectPrevious() override { commands.push_back("SelectPrevious"); }
        void BrowserCmdSelectNext() override { commands.push_back("SelectNext"); }
        void BrowserCmdSelectFirst() override { commands.push_back("SelectFirst"); }
        void BrowserCmdSelectLast() override { commands.push_back("SelectLast"); }
        void BrowserCmdPagePrevious() override { commands.push_back("PagePrevious"); }
        void BrowserCmdPageNext() override { commands.push_back("PageNext"); }
        void BrowserCmdNavigateUp() override { commands.push_back("NavigateUp"); }
        void BrowserCmdFitToScreen() override { commands.push_back("FitToScreen"); }
        void BrowserCmdToggleDirectories() override { commands.push_back("ToggleDirectories"); }
        void BrowserCmdToggleAlpha() override { commands.push_back("ToggleAlpha"); }
        void BrowserCmdToggleSampling() override { commands.push_back("ToggleSampling"); }
        void BrowserCmdRotateLeft() override { commands.push_back("RotateLeft"); }
        void BrowserCmdRotateRight() override { commands.push_back("RotateRight"); }
        void BrowserCmdRotate180() override { commands.push_back("Rotate180"); }
        void BrowserCmdRotateReset() override { commands.push_back("RotateReset"); }
        void BrowserCmdMipPrevious() override { commands.push_back("MipPrevious"); }
        void BrowserCmdMipNext() override { commands.push_back("MipNext"); }
        void BrowserCmdSelectMip(uint32_t) override { commands.push_back("SelectMip"); }
        void BrowserCmdSetChannelMode(int) override { commands.push_back("SetChannelMode"); }
        void BrowserCmdSetAlphaUsageOverride(ImageCore::AlphaUsage) override { commands.push_back("SetAlphaUsageOverride"); }
        void BrowserCmdShowImageInformation() override { commands.push_back("ShowImageInformation"); }

        bool BrowserTryStartCompareWithFileNameMatch(const std::wstring&) override { return false; }
        void BrowserRestoreOpenFile(const std::wstring&) override {}
        void BrowserOpenAdditionalFileInHorizontalSplit(const std::wstring&) override {}
        void BrowserOpenAdditionalFilesSideBySideAfterName(const std::vector<std::wstring>&, const std::wstring&) override {}
        void BrowserRestoreOpenFolder(const std::wstring&) override {}
        void BrowserAddHorizontalViewerForRestore(const std::wstring&) override {}
        void BrowserAddHorizontalViewerForRestoreFolder(const std::wstring&) override {}
        void BrowserForceApplySyncedThumbStripHeight() override {}
        void BrowserApplyHorizontalSplitRatios(const std::vector<float>&) override {}

        void BrowserSelectFileNameForSync(const std::wstring&) override {}
        std::wstring BrowserGetRequestedFileNameLower() const override { return {}; }
        void BrowserApplyViewTransformForSync(const ImageViewTransform&) override {}
        void BrowserApplyShowNavItemsForSync(bool) override {}
        void BrowserApplyBackgroundColorForSync(const D2D1_COLOR_F&) override {}
        void BrowserApplyFocusedBackgroundColorForSync(const D2D1_COLOR_F&) override {}
        void BrowserApplyAlphaCheckerboardForSync(bool) override {}

        std::wstring BrowserGetRequestedFilePath() const override { return {}; }
        std::wstring BrowserGetDisplayedFilePath() const override { return {}; }
        std::wstring BrowserGetCurrentFolderPath() const override { return {}; }
        bool BrowserTryGetMainImageClientRect(D2D1_RECT_F&) const override { return false; }
        std::vector<float> BrowserCaptureHorizontalSplitRatios() const override { return {}; }
        bool BrowserContextMenuPrepareForDisplay(const POINT&) override { return false; }
        ContextMenuSnapshot BrowserContextMenuSnapshotAtPoint(const POINT&) const override { return {}; }
        std::wstring BrowserGetExplorerTargetPathAtPoint(const POINT&) const override { return {}; }
        bool BrowserHasFocusForTitle() const override { return false; }
        std::wstring BrowserRequestedImageFileNameForTitle() const override { return {}; }
    };

    struct Modifiers
    {
        bool ctrl { false };
        bool shift { false };
        bool alt { false };
    };
    constexpr Modifiers kNone {};
    constexpr Modifiers kCtrl { true, false, false };
    constexpr Modifiers kShift { false, true, false };
    constexpr Modifiers kAlt { false, false, true };
    constexpr Modifiers kCtrlShift { true, true, false };
    constexpr Modifiers kCtrlAlt { true, false, true };

    // The commands a key-up runs ("" when it is not handled).
    std::string KeyUp(Ficture2Backplate& backplate, RecordingBrowser& browser, UINT key, Modifiers m)
    {
        bool handled = false;
        const bool consumed = backplate.HandleImageBrowserKeyUpCommand(&browser, key, m.ctrl, m.shift, m.alt, handled);
        const std::string commands = browser.Take();
        return (consumed == handled && handled == !commands.empty()) ? commands : "mismatch:" + commands;
    }

    std::string KeyDown(Ficture2Backplate& backplate, RecordingBrowser& browser, UINT key, Modifiers m)
    {
        bool handled = false;
        const bool consumed = backplate.HandleImageBrowserKeyDownCommand(&browser, key, m.ctrl, m.shift, m.alt, handled);
        const std::string commands = browser.Take();
        return (consumed == handled && handled == !commands.empty()) ? commands : "mismatch:" + commands;
    }
}

FIC2_TEST(BackplateCommands_CtrlBracketsStepMipLevels)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    RecordingBrowser browser;

    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_4, kCtrl) == "MipPrevious");
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_6, kCtrl) == "MipNext");
    // Only with Ctrl alone.
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_4, kNone).empty());
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_6, kNone).empty());
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_4, kCtrlShift).empty());
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_6, kCtrlAlt).empty());
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_4, kAlt).empty());
}

FIC2_TEST(BackplateCommands_KeyUpBindings)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    RecordingBrowser browser;

    // Selection and navigation (any modifiers).
    FIC2_CHECK(KeyUp(backplate, browser, VK_RETURN, kNone) == "ActivateSelected");
    FIC2_CHECK(KeyUp(backplate, browser, VK_HOME, kNone) == "SelectFirst");
    FIC2_CHECK(KeyUp(backplate, browser, VK_END, kNone) == "SelectLast");
    FIC2_CHECK(KeyUp(backplate, browser, VK_PRIOR, kNone) == "PagePrevious");
    FIC2_CHECK(KeyUp(backplate, browser, VK_NEXT, kNone) == "PageNext");
    FIC2_CHECK(KeyUp(backplate, browser, VK_BACK, kNone) == "NavigateUp");
    FIC2_CHECK(KeyUp(backplate, browser, VK_HOME, kCtrl) == "SelectFirst");

    // Lifecycle chords.
    FIC2_CHECK(KeyUp(backplate, browser, VK_F4, kCtrl) == "Close");
    FIC2_CHECK(KeyUp(backplate, browser, VK_F4, kCtrlShift) == "Close");
    FIC2_CHECK(KeyUp(backplate, browser, VK_F4, kNone).empty());

    // Alt chords (letters case-insensitively; Ctrl is ignored for these).
    FIC2_CHECK(KeyUp(backplate, browser, VK_UP, kAlt) == "NavigateUp");
    FIC2_CHECK(KeyUp(backplate, browser, 'N', kAlt) == "ToggleDirectories");
    FIC2_CHECK(KeyUp(backplate, browser, 'n', kAlt) == "ToggleDirectories");
    FIC2_CHECK(KeyUp(backplate, browser, 'N', kCtrlAlt) == "ToggleDirectories");
    FIC2_CHECK(KeyUp(backplate, browser, 'A', kAlt) == "ToggleAlpha");
    FIC2_CHECK(KeyUp(backplate, browser, 'X', kAlt) == "FitToScreen");
    FIC2_CHECK(KeyUp(backplate, browser, 'Q', kAlt) == "ToggleSampling");
    FIC2_CHECK(KeyUp(backplate, browser, 'N', kNone).empty());

    // < / > rotate.
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_COMMA, kShift) == "RotateLeft");
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_PERIOD, kShift) == "RotateRight");
    FIC2_CHECK(KeyUp(backplate, browser, VK_OEM_COMMA, kNone).empty());

    // Arrows move on key down, not key up.
    FIC2_CHECK(KeyUp(backplate, browser, VK_LEFT, kNone).empty());
}

FIC2_TEST(BackplateCommands_KeyDownBindings)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    RecordingBrowser browser;

    FIC2_CHECK(KeyDown(backplate, browser, VK_LEFT, kNone) == "SelectPrevious");
    FIC2_CHECK(KeyDown(backplate, browser, VK_RIGHT, kNone) == "SelectNext");
    FIC2_CHECK(KeyDown(backplate, browser, VK_RIGHT, kShift) == "SelectNext");
    FIC2_CHECK(KeyDown(backplate, browser, VK_HOME, kNone).empty());
    FIC2_CHECK(KeyDown(backplate, browser, VK_OEM_4, kCtrl).empty());

    // A window that is not an image browser runs nothing.
    FD2D::Wnd other(L"not-a-browser");
    bool handled = true;
    FIC2_CHECK(!backplate.HandleImageBrowserKeyDownCommand(&other, VK_LEFT, false, false, false, handled));
    FIC2_CHECK(!handled);
}

FIC2_TEST(BackplateCommands_ContextMenuCommands)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    RecordingBrowser browser;
    const auto run = [&](UINT id)
    {
        const bool handled = backplate.HandleImageBrowserContextMenuCommand(&browser, id, POINT {});
        const std::string commands = browser.Take();
        return handled ? commands : "unhandled:" + commands;
    };

    FIC2_CHECK(run(IDM_CTX_CLOSE) == "Close");
    FIC2_CHECK(run(IDM_CTX_FIT_TO_SCREEN) == "FitToScreen");
    FIC2_CHECK(run(IDM_CTX_TOGGLE_DIRECTORIES) == "ToggleDirectories");
    FIC2_CHECK(run(IDM_CTX_TOGGLE_ALPHA) == "ToggleAlpha");
    FIC2_CHECK(run(IDM_CTX_TOGGLE_SAMPLING) == "ToggleSampling");
    FIC2_CHECK(run(IDM_CTX_ROTATE_LEFT) == "RotateLeft");
    FIC2_CHECK(run(IDM_CTX_ROTATE_RIGHT) == "RotateRight");
    FIC2_CHECK(run(IDM_CTX_ROTATE_180) == "Rotate180");
    FIC2_CHECK(run(IDM_CTX_ROTATE_RESET) == "RotateReset");
    FIC2_CHECK(run(0) == "unhandled:");
}
