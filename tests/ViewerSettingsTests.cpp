#include "TestHarness.h"

#include "AppSetup.h"
#include "Ficture2Backplate.h"
#include "IniStore.h"
#include "ViewerSettings.h"

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

// The viewer settings shared by a window's image browsers: defaults, loading
// once from the INI, colour parsing, and the keys each setter writes.

namespace
{
    struct TempIni
    {
        std::filesystem::path dir {};
        std::wstring path {};

        TempIni()
        {
            static int counter = 0;
            wchar_t base[MAX_PATH] {};
            GetTempPathW(MAX_PATH, base);
            dir = std::filesystem::path(base) /
                (L"FICture2ViewerSettingsTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(++counter));
            std::filesystem::create_directories(dir);
            path = (dir / L"FICture2.ini").wstring();
        }

        ~TempIni()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const char* text) const
        {
            std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            out << text;
        }

        std::wstring Get(const wchar_t* section, const wchar_t* key) const
        {
            return IniStore::Load(path).GetString(section, key);
        }
    };

    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    }

    bool SameColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b)
    {
        return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a);
    }

    D2D1_COLOR_F Rgb(int r, int g, int b)
    {
        return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    bool HasDefaults(const ViewerSettings& settings)
    {
        return settings.ShowNavItems() && !settings.AlphaCheckerboard() &&
            SameColor(settings.FocusedBackgroundColor(), ViewerSettings::kDefaultFocusedBackgroundColor) &&
            Near(settings.ZoomStiffness(), ViewerSettings::kDefaultZoomStiffness);
    }
}

FIC2_TEST(ViewerSettings_DefaultsWithoutAFile)
{
    ViewerSettings unset;
    FIC2_CHECK(!unset.Loaded());
    FIC2_CHECK(HasDefaults(unset));

    ViewerSettings noPath;
    FIC2_CHECK(!noPath.Load(L"").has_value());
    FIC2_CHECK(noPath.Loaded());
    FIC2_CHECK(HasDefaults(noPath));

    const TempIni ini;   // the file does not exist
    ViewerSettings missing;
    FIC2_CHECK(!missing.Load(ini.path).has_value());
    FIC2_CHECK(missing.Loaded());
    FIC2_CHECK(HasDefaults(missing));
}

FIC2_TEST(ViewerSettings_LoadsEachKeyOnce)
{
    const TempIni ini;
    ini.Write(
        "[Viewer]\r\nShowNavItems=0\r\n"
        "[Window]\r\nBackgroundColor=20,23,23\r\nFocusedBackgroundColor=35,43,43\r\n"
        "[Image]\r\nZoomStiffness=150\r\n");

    ViewerSettings settings;
    const auto background = settings.Load(ini.path);
    FIC2_REQUIRE(background.has_value());
    FIC2_CHECK(SameColor(*background, Rgb(20, 23, 23)));
    FIC2_CHECK(!settings.ShowNavItems());
    FIC2_CHECK(SameColor(settings.FocusedBackgroundColor(), Rgb(35, 43, 43)));
    FIC2_CHECK(Near(settings.ZoomStiffness(), 150.0f));

    // Loaded once: a changed file is not read again.
    ini.Write("[Viewer]\r\nShowNavItems=1\r\n[Window]\r\nBackgroundColor=1,2,3\r\n");
    FIC2_CHECK(!settings.Load(ini.path).has_value());
    FIC2_CHECK(!settings.ShowNavItems());

    // Marked loaded (tests): the file is never read.
    ViewerSettings skipped;
    skipped.MarkLoaded();
    FIC2_CHECK(!skipped.Load(ini.path).has_value());
    FIC2_CHECK(HasDefaults(skipped));
}

FIC2_TEST(ViewerSettings_InvalidValuesKeepTheDefaults)
{
    const TempIni ini;
    ini.Write(
        "[Window]\r\nBackgroundColor=red\r\nFocusedBackgroundColor=1,2\r\n"
        "[Image]\r\nZoomStiffness=9.5\r\n");
    ViewerSettings low;
    FIC2_CHECK(!low.Load(ini.path).has_value());
    FIC2_CHECK(HasDefaults(low));

    ini.Write("[Image]\r\nZoomStiffness=500.5\r\n");
    ViewerSettings high;
    (void)high.Load(ini.path);
    FIC2_CHECK(Near(high.ZoomStiffness(), ViewerSettings::kDefaultZoomStiffness));

    // The range ends are accepted.
    ini.Write("[Image]\r\nZoomStiffness=10\r\n");
    ViewerSettings edge;
    (void)edge.Load(ini.path);
    FIC2_CHECK(Near(edge.ZoomStiffness(), 10.0f));
}

FIC2_TEST(ViewerSettings_ColorParsingAndFormat)
{
    D2D1_COLOR_F color {};
    FIC2_CHECK(ViewerSettings::ParseRgb(L"0,128,255", color));
    FIC2_CHECK(SameColor(color, Rgb(0, 128, 255)));
    // Components are clamped to 0-255.
    FIC2_CHECK(ViewerSettings::ParseRgb(L"-5,300,7", color));
    FIC2_CHECK(SameColor(color, Rgb(0, 255, 7)));
    FIC2_CHECK(!ViewerSettings::ParseRgb(L"", color));
    FIC2_CHECK(!ViewerSettings::ParseRgb(L"1,2", color));
    FIC2_CHECK(!ViewerSettings::ParseRgb(L"#ff0000", color));

    FIC2_CHECK(ViewerSettings::FormatRgb(Rgb(20, 23, 23)) == L"20,23,23");
    FIC2_CHECK(ViewerSettings::FormatRgb(D2D1::ColorF(0.0f, 1.0f, 0.5f, 1.0f)) == L"0,255,128");
}

FIC2_TEST(ViewerSettings_SettersWriteTheirKeys)
{
    const TempIni ini;
    ViewerSettings settings;
    (void)settings.Load(ini.path);

    settings.SetShowNavItems(false, ini.path);
    settings.SetFocusedBackgroundColor(Rgb(1, 2, 3), ini.path);
    ViewerSettings::SaveBackgroundColor(Rgb(4, 5, 6), ini.path);
    settings.SetAlphaCheckerboard(true);
    FIC2_CHECK(!settings.ShowNavItems());
    FIC2_CHECK(SameColor(settings.FocusedBackgroundColor(), Rgb(1, 2, 3)));
    FIC2_CHECK(settings.AlphaCheckerboard());

    FIC2_CHECK(ini.Get(L"Viewer", L"ShowNavItems") == L"0");
    FIC2_CHECK(ini.Get(L"Window", L"FocusedBackgroundColor") == L"1,2,3");
    FIC2_CHECK(ini.Get(L"Window", L"BackgroundColor") == L"4,5,6");
    // The checkerboard is not saved.
    FIC2_CHECK(ini.Get(L"Viewer", L"AlphaCheckerboard").empty());

    // A new load reads back what was written.
    ViewerSettings reloaded;
    const auto background = reloaded.Load(ini.path);
    FIC2_REQUIRE(background.has_value());
    FIC2_CHECK(SameColor(*background, Rgb(4, 5, 6)));
    FIC2_CHECK(!reloaded.ShowNavItems());
    FIC2_CHECK(SameColor(reloaded.FocusedBackgroundColor(), Rgb(1, 2, 3)));
    FIC2_CHECK(!reloaded.AlphaCheckerboard());

    settings.SetShowNavItems(true, ini.path);
    FIC2_CHECK(ini.Get(L"Viewer", L"ShowNavItems") == L"1");

    // Without a path nothing is written, but the value changes.
    settings.SetShowNavItems(false, L"");
    FIC2_CHECK(!settings.ShowNavItems());
    FIC2_CHECK(ini.Get(L"Viewer", L"ShowNavItems") == L"1");
}

// The window loads the settings from the INI once, applies the saved
// background as its clear colour and hands the rest to its browsers. Uses the
// test run's own INI (restored afterwards).
FIC2_TEST(ViewerSettings_BackplateLoadsTheIniOnce)
{
    const std::filesystem::path iniPath = FICture2App::GetIniFilePath();
    FIC2_REQUIRE(!iniPath.empty());
    std::optional<std::string> saved {};
    {
        std::ifstream in(iniPath, std::ios::binary);
        if (in)
        {
            saved = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    {
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
        out << "[Viewer]\r\nShowNavItems=0\r\n"
               "[Window]\r\nBackgroundColor=20,23,23\r\nFocusedBackgroundColor=35,43,43\r\n"
               "[Image]\r\nZoomStiffness=150\r\n";
    }

    Ficture2Backplate backplate(L"viewer-settings-test");
    backplate.EnsureImageBrowserIniInitialized();
    FIC2_CHECK(!backplate.ShowNavItemsEnabled());
    FIC2_CHECK(SameColor(backplate.ClearColor(), Rgb(20, 23, 23)));
    FIC2_CHECK(SameColor(backplate.FocusedBackgroundColor(), Rgb(35, 43, 43)));
    FIC2_CHECK(Near(backplate.ImageZoomStiffness(), 150.0f));
    FIC2_CHECK(!backplate.AlphaCheckerboardEnabled());

    // Once per window: a later change of the file is not picked up.
    {
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
        out << "[Viewer]\r\nShowNavItems=1\r\n";
    }
    backplate.EnsureImageBrowserIniInitialized();
    FIC2_CHECK(!backplate.ShowNavItemsEnabled());

    // The test seam keeps the defaults.
    Ficture2Backplate skipped(L"viewer-settings-skip");
    skipped.SkipImageBrowserIniForTesting();
    skipped.EnsureImageBrowserIniInitialized();
    FIC2_CHECK(skipped.ShowNavItemsEnabled());
    FIC2_CHECK(Near(skipped.ImageZoomStiffness(), ViewerSettings::kDefaultZoomStiffness));

    std::error_code ec;
    if (saved)
    {
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
        out << *saved;
    }
    else
    {
        std::filesystem::remove(iniPath, ec);
    }
}
