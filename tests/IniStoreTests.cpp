#include "TestHarness.h"

#include "IniStore.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// IniStore on temporary INI files only (never the user's FICture2.ini).

namespace
{
    // A unique folder under %TEMP%, removed with its contents on destruction.
    struct TempDir
    {
        std::filesystem::path dir {};

        TempDir()
        {
            static int counter = 0;
            wchar_t base[MAX_PATH] {};
            GetTempPathW(MAX_PATH, base);
            dir = std::filesystem::path(base) /
                (L"FICture2IniStoreTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter));
            std::filesystem::create_directories(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        std::wstring File(const wchar_t* name) const
        {
            return (dir / name).wstring();
        }
    };

    void WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        (void)WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    }

    // UTF-16LE with a BOM, the form IniStore writes.
    std::vector<std::uint8_t> Utf16Bytes(const std::wstring& text)
    {
        std::vector<std::uint8_t> bytes { 0xFF, 0xFE };
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        bytes.insert(bytes.end(), data, data + text.size() * sizeof(wchar_t));
        return bytes;
    }
}

// ';' is legal in file and folder names: a value keeps everything after '=',
// and only a line that starts with ';' or '#' is a comment.
FIC2_TEST(IniStore_ValueKeepsSemicolons)
{
    const TempDir temp;
    const std::wstring ini = temp.File(L"semicolons.ini");
    WriteBytes(ini, Utf16Bytes(
        L"; full-line comment\r\n"
        L"# hash comment\r\n"
        L"[Session]\r\n"
        L"RecentFiles=C:\\a;b\\x.png|C:\\c\\y.png|C:\\d;\\z.png\r\n"
        L"  ; Indented=comment\r\n"
        L"[Viewer0]\r\n"
        L"DisplayedFile=C:\\dir;semi\\img.png\r\n"
        L"CurrentFolder = C:\\dir;semi ; not a comment\r\n"));

    const IniStore store = IniStore::Load(ini);
    FIC2_REQUIRE(store.IsLoaded());
    FIC2_CHECK(store.GetString(L"Viewer0", L"DisplayedFile") == L"C:\\dir;semi\\img.png");
    FIC2_CHECK(store.GetString(L"Viewer0", L"CurrentFolder") == L"C:\\dir;semi ; not a comment");
    FIC2_CHECK(store.GetString(L"Session", L"; Indented", L"none") == L"none");

    const std::vector<std::wstring> recent = IniStore::SplitPipeList(store.GetString(L"Session", L"RecentFiles"));
    FIC2_REQUIRE(recent.size() == 3);
    FIC2_CHECK(recent[0] == L"C:\\a;b\\x.png");
    FIC2_CHECK(recent[1] == L"C:\\c\\y.png");
    FIC2_CHECK(recent[2] == L"C:\\d;\\z.png");
}
