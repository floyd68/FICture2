#include "TestHarness.h"

#include "AppDialogs.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// The dialog service's pure parts: the open-file filter and the folder a
// file dialog starts in. (The native dialogs are modal; commands are tested
// against fake dialogs in BackplateCommandTests.)

namespace
{
    // The filter's null-separated parts (the final empty part ends it).
    std::vector<std::wstring> Parts(const std::wstring& filter)
    {
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (size_t i = 0; i < filter.size(); ++i)
        {
            if (filter[i] == L'\0')
            {
                parts.push_back(filter.substr(start, i - start));
                start = i + 1;
            }
        }
        return parts;
    }
}

FIC2_TEST(AppDialogs_SupportedImageFilter)
{
    const std::wstring filter = AppDialogs::SupportedImageFilter({ L".png", L".dds", L".jpg" });
    FIC2_CHECK((Parts(filter) == std::vector<std::wstring> {
        L"Supported images (*.png;*.dds;*.jpg)", L"*.png;*.dds;*.jpg", L"All files (*.*)", L"*.*", L"" }));
    // Double-null terminated.
    FIC2_REQUIRE(filter.size() >= 2);
    FIC2_CHECK(filter[filter.size() - 1] == L'\0' && filter[filter.size() - 2] == L'\0');

    FIC2_CHECK((Parts(AppDialogs::SupportedImageFilter({ L".png" })) == std::vector<std::wstring> {
        L"Supported images (*.png)", L"*.png", L"All files (*.*)", L"*.*", L"" }));
    // No extensions: every file.
    FIC2_CHECK((Parts(AppDialogs::SupportedImageFilter({})) == std::vector<std::wstring> {
        L"Supported images (*.*)", L"*.*", L"All files (*.*)", L"*.*", L"" }));
}

FIC2_TEST(AppDialogs_InitialDirIsAnExistingFolder)
{
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    FIC2_CHECK(AppDialogs::InitialDir(temp.wstring()) == temp.wstring());
    FIC2_CHECK(AppDialogs::InitialDir((temp / L"FICture2-no-such-folder").wstring()).empty());
    FIC2_CHECK(AppDialogs::InitialDir(L"").empty());

    // A file is not a folder.
    const std::filesystem::path file = temp / (L"FICture2AppDialogsTests-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    {
        FILE* f = nullptr;
        FIC2_REQUIRE(_wfopen_s(&f, file.c_str(), L"wb") == 0 && f != nullptr);
        fclose(f);
    }
    FIC2_CHECK(AppDialogs::InitialDir(file.wstring()).empty());
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// The About dialog's icon is loaded for one dialog and released when it
// returns (also when it fails), so repeated About dialogs keep no icons.
FIC2_TEST(AppDialogs_AboutReleasesItsIcon)
{
    const HANDLE process = GetCurrentProcess();
    int shown = 0;
    int iconsValidWhileShown = 0;
    const auto show = [&](HRESULT result)
    {
        return [&, result](const TASKDIALOGCONFIG& config)
        {
            ++shown;
            FIC2_CHECK(config.cbSize == sizeof(TASKDIALOGCONFIG));
            FIC2_CHECK((config.dwFlags & TDF_USE_HICON_MAIN) != 0);
            FIC2_CHECK(std::wstring(config.pszWindowTitle) == L"About FICture2");
            FIC2_CHECK(std::wstring(config.pszContent).find(L"Version: ") == 0);
            ICONINFO info {};
            if (config.hMainIcon != nullptr && GetIconInfo(config.hMainIcon, &info))
            {
                ++iconsValidWhileShown;
                DeleteObject(info.hbmColor);
                DeleteObject(info.hbmMask);
            }
            return result;
        };
    };

    // The first call may create per-process USER state; count from after it.
    FIC2_CHECK(AppDialogs::ShowAboutWith(nullptr, show(S_OK)) == S_OK);
    const DWORD before = GetGuiResources(process, GR_USEROBJECTS);
    for (int i = 0; i < 25; ++i)
    {
        FIC2_CHECK(AppDialogs::ShowAboutWith(nullptr, show(S_OK)) == S_OK);
        FIC2_CHECK(AppDialogs::ShowAboutWith(nullptr, show(E_FAIL)) == E_FAIL);
    }
    const DWORD after = GetGuiResources(process, GR_USEROBJECTS);

    FIC2_CHECK(shown == 51);
    FIC2_CHECK(iconsValidWhileShown == 51);   // the dialog gets the app icon
    std::printf("  USER objects: %lu before, %lu after 50 About dialogs\n", before, after);
    FIC2_CHECK(after == before);
}
