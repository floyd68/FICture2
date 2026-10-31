#include "TestHarness.h"

#include "AppDialogs.h"
#include "AppSetup.h"
#include "Ficture2Backplate.h"
#include "FD2D/Wnd.h"
#include "ImageBrowser.h"
#include "Resource.h"

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The window's key and context-menu command dispatch: which browser command a
// key (with its modifiers) or a menu item runs. Commands that open dialogs
// (open, colour pickers, about) run against fake dialogs.

namespace
{
    // An image browser that records the commands it is asked to run.
    class RecordingBrowser final : public FD2D::Wnd, public IImageBrowserOps
    {
    public:
        RecordingBrowser() : FD2D::Wnd(L"recording-browser") {}

        std::vector<std::string> commands {};
        std::wstring lastPath {};          // of the last open command
        std::wstring lastAfterName {};     // of the last side-by-side open
        std::wstring currentFolder {};     // what BrowserGetCurrentFolderPath reports

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
        void BrowserRestoreOpenFile(const std::wstring& path) override { Opened("RestoreOpenFile", path); }
        void BrowserOpenAdditionalFileInHorizontalSplit(const std::wstring& path) override { Opened("OpenInSplit", path); }
        void BrowserOpenAdditionalFilesSideBySideAfterName(const std::vector<std::wstring>& paths, const std::wstring& afterName) override
        {
            Opened("OpenSideBySide", paths.size() == 1 ? paths[0] : L"");
            lastAfterName = afterName;
        }
        void BrowserRestoreOpenFolder(const std::wstring& path) override { Opened("RestoreOpenFolder", path); }
        void BrowserAddHorizontalViewerForRestore(const std::wstring&) override {}
        void BrowserAddHorizontalViewerForRestoreFolder(const std::wstring&) override {}
        void BrowserForceApplySyncedThumbStripHeight() override {}
        void BrowserApplyHorizontalSplitRatios(const std::vector<float>&) override {}

        void BrowserSelectFileNameForSync(const std::wstring&) override {}
        std::wstring BrowserGetRequestedFileNameLower() const override { return {}; }
        void BrowserApplyViewTransformForSync(const ImageViewTransform&) override {}
        void BrowserApplyShowNavItemsForSync(bool) override {}
        void BrowserApplyBackgroundColorForSync(const D2D1_COLOR_F&) override { commands.push_back("ApplyBackgroundColor"); }
        void BrowserApplyFocusedBackgroundColorForSync(const D2D1_COLOR_F&) override { commands.push_back("ApplyFocusedBackgroundColor"); }
        void BrowserApplyAlphaCheckerboardForSync(bool) override {}

        std::wstring BrowserGetRequestedFilePath() const override { return {}; }
        std::wstring BrowserGetDisplayedFilePath() const override { return {}; }
        std::wstring BrowserGetCurrentFolderPath() const override { return currentFolder; }
        bool BrowserTryGetMainImageClientRect(D2D1_RECT_F&) const override { return false; }
        std::vector<float> BrowserCaptureHorizontalSplitRatios() const override { return {}; }
        bool BrowserContextMenuPrepareForDisplay(const POINT&) override { return false; }
        ContextMenuSnapshot BrowserContextMenuSnapshotAtPoint(const POINT&) const override { return {}; }
        std::wstring BrowserGetExplorerTargetPathAtPoint(const POINT&) const override { return {}; }
        bool BrowserHasFocusForTitle() const override { return false; }
        std::wstring BrowserRequestedImageFileNameForTitle() const override { return {}; }

    private:
        void Opened(const char* command, const std::wstring& path)
        {
            commands.push_back(command);
            lastPath = path;
        }
    };

    // Dialogs that answer at once with a prepared result (none: cancelled)
    // and record how they were asked.
    class FakeDialogs final : public AppDialogs
    {
    public:
        std::optional<std::wstring> file {};
        std::optional<std::wstring> folder {};
        std::optional<D2D1_COLOR_F> color {};

        std::vector<std::string> asked {};
        std::wstring lastTitle {};
        std::wstring lastInitialDir {};
        D2D1_COLOR_F lastCurrentColor {};

        bool PickImageFile(HWND, const std::wstring& initialDir, const wchar_t* title, std::wstring& outPath) override
        {
            asked.push_back("PickImageFile");
            lastTitle = (title != nullptr) ? title : L"";
            lastInitialDir = initialDir;
            if (file)
            {
                outPath = *file;
            }
            return file.has_value();
        }

        bool PickFolder(HWND, std::wstring& outFolder) override
        {
            asked.push_back("PickFolder");
            if (folder)
            {
                outFolder = *folder;
            }
            return folder.has_value();
        }

        bool PickColor(HWND, const D2D1_COLOR_F& current, D2D1_COLOR_F& outColor) override
        {
            asked.push_back("PickColor");
            lastCurrentColor = current;
            if (color)
            {
                outColor = *color;
            }
            return color.has_value();
        }

        void ShowAbout(HWND) override { asked.push_back("ShowAbout"); }

        std::string Take()
        {
            std::string all {};
            for (const auto& a : asked)
            {
                all += (all.empty() ? "" : ",") + a;
            }
            asked.clear();
            return all;
        }
    };

    bool SameColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b)
    {
        return std::fabs(a.r - b.r) < 0.001f && std::fabs(a.g - b.g) < 0.001f &&
            std::fabs(a.b - b.b) < 0.001f && std::fabs(a.a - b.a) < 0.001f;
    }

    // Keeps the test run's INI as it was (colour commands save their colour).
    class RestoreTestIni
    {
    public:
        RestoreTestIni() : m_path(FICture2App::GetIniFilePath())
        {
            std::ifstream in(m_path, std::ios::binary);
            if (in)
            {
                m_saved = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
        }

        ~RestoreTestIni()
        {
            if (m_saved)
            {
                std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
                out << *m_saved;
            }
            else
            {
                std::error_code ec;
                std::filesystem::remove(m_path, ec);
            }
        }

    private:
        std::filesystem::path m_path {};
        std::optional<std::string> m_saved {};
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

FIC2_TEST(BackplateCommands_OpenCommandsUseThePickedPath)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    auto dialogs = std::make_shared<FakeDialogs>();
    backplate.SetDialogsForTesting(dialogs);
    RecordingBrowser browser;
    const auto menu = [&](UINT id)
    {
        FIC2_CHECK(backplate.HandleImageBrowserContextMenuCommand(&browser, id, POINT {}));
        return browser.Take();
    };

    // Cancelled dialogs do nothing.
    FIC2_CHECK(menu(IDM_CTX_OPEN_IMAGE).empty());
    FIC2_CHECK(menu(IDM_CTX_OPEN_NEW_IMAGE).empty());
    FIC2_CHECK(menu(IDM_CTX_OPEN_FOLDER).empty());
    FIC2_CHECK(dialogs->Take() == "PickImageFile,PickImageFile,PickFolder");

    dialogs->file = L"C:\\Pictures\\a.png";
    dialogs->folder = L"C:\\Pictures";

    // The file dialog starts in the browser's folder when it exists.
    const std::filesystem::path existing = std::filesystem::temp_directory_path();
    browser.currentFolder = existing.wstring();
    FIC2_CHECK(menu(IDM_CTX_OPEN_IMAGE) == "RestoreOpenFile");
    FIC2_CHECK(browser.lastPath == L"C:\\Pictures\\a.png");
    FIC2_CHECK(dialogs->lastTitle == L"Open Image");
    FIC2_CHECK(dialogs->lastInitialDir == existing.wstring());
    browser.currentFolder = (existing / L"FICture2-no-such-folder").wstring();
    FIC2_CHECK(menu(IDM_CTX_OPEN_IMAGE) == "RestoreOpenFile");
    FIC2_CHECK(dialogs->lastInitialDir.empty());

    // Ctrl+O and Ctrl+Shift+O (key up).
    bool handled = false;
    FIC2_CHECK(backplate.HandleImageBrowserKeyUpCommand(&browser, 'O', true, false, false, handled) && handled);
    FIC2_CHECK(browser.Take() == "RestoreOpenFile");
    FIC2_CHECK(backplate.HandleImageBrowserKeyUpCommand(&browser, 'O', true, true, false, handled) && handled);
    FIC2_CHECK(browser.Take() == "OpenInSplit");
    FIC2_CHECK(dialogs->lastTitle == L"Open Image");

    // Open New Image: beside the source browser, while there is room.
    FIC2_CHECK(menu(IDM_CTX_OPEN_NEW_IMAGE) == "OpenSideBySide");
    FIC2_CHECK(browser.lastPath == L"C:\\Pictures\\a.png");
    FIC2_CHECK(browser.lastAfterName == browser.Name());
    FIC2_CHECK(dialogs->lastTitle == L"Open New Image");

    std::vector<std::unique_ptr<FD2D::Wnd>> registered {};
    for (int i = 0; i < 8; ++i)
    {
        registered.push_back(std::make_unique<FD2D::Wnd>(L"viewer" + std::to_wstring(i)));
        backplate.Registry().RegisterImageBrowser(registered.back().get());
    }
    FIC2_CHECK(menu(IDM_CTX_OPEN_NEW_IMAGE).empty());
    for (const auto& wnd : registered)
    {
        backplate.Registry().UnregisterImageBrowser(wnd.get());
    }

    FIC2_CHECK(menu(IDM_CTX_OPEN_FOLDER) == "RestoreOpenFolder");
    FIC2_CHECK(browser.lastPath == L"C:\\Pictures");
}

FIC2_TEST(BackplateCommands_ColorCommandsApplyThePickedColor)
{
    const RestoreTestIni restoreIni;
    Ficture2Backplate backplate(L"backplate-commands-test");
    backplate.SkipImageBrowserIniForTesting();
    auto dialogs = std::make_shared<FakeDialogs>();
    backplate.SetDialogsForTesting(dialogs);
    RecordingBrowser browser;
    const auto menu = [&](UINT id)
    {
        FIC2_CHECK(backplate.HandleImageBrowserContextMenuCommand(&browser, id, POINT {}));
        return browser.Take();
    };

    const D2D1_COLOR_F clearBefore = backplate.ClearColor();
    const D2D1_COLOR_F focusedBefore = backplate.FocusedBackgroundColor();

    // Cancelled: nothing changes; the dialog starts from the current colour.
    FIC2_CHECK(menu(IDM_CTX_BACKGROUND_COLOR).empty());
    FIC2_CHECK(SameColor(dialogs->lastCurrentColor, clearBefore));
    FIC2_CHECK(menu(IDM_CTX_FOCUSED_BACKGROUND_COLOR).empty());
    FIC2_CHECK(SameColor(dialogs->lastCurrentColor, focusedBefore));
    FIC2_CHECK(SameColor(backplate.ClearColor(), clearBefore));
    FIC2_CHECK(dialogs->Take() == "PickColor,PickColor");

    const D2D1_COLOR_F picked = D2D1::ColorF(0.2f, 0.4f, 0.6f, 1.0f);
    dialogs->color = picked;
    FIC2_CHECK(menu(IDM_CTX_BACKGROUND_COLOR) == "ApplyBackgroundColor");
    FIC2_CHECK(SameColor(backplate.ClearColor(), picked));
    FIC2_CHECK(menu(IDM_CTX_FOCUSED_BACKGROUND_COLOR) == "ApplyFocusedBackgroundColor");
    FIC2_CHECK(SameColor(backplate.FocusedBackgroundColor(), picked));

    // Alt+B is the background colour too.
    const D2D1_COLOR_F other = D2D1::ColorF(0.8f, 0.0f, 0.0f, 1.0f);
    dialogs->color = other;
    bool handled = false;
    FIC2_CHECK(backplate.HandleImageBrowserKeyUpCommand(&browser, 'B', false, false, true, handled) && handled);
    FIC2_CHECK(browser.Take() == "ApplyBackgroundColor");
    FIC2_CHECK(SameColor(backplate.ClearColor(), other));
}

FIC2_TEST(BackplateCommands_AboutUsesTheDialogService)
{
    Ficture2Backplate backplate(L"backplate-commands-test");
    auto dialogs = std::make_shared<FakeDialogs>();
    backplate.SetDialogsForTesting(dialogs);
    RecordingBrowser browser;
    FIC2_CHECK(backplate.HandleImageBrowserContextMenuCommand(&browser, IDM_CTX_ABOUT, POINT {}));
    FIC2_CHECK(dialogs->Take() == "ShowAbout");
    FIC2_CHECK(browser.Take().empty());
}
