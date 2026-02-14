// FICture2.cpp : application entrypoint

#include "FICture2.h"

#include "Ficture2Backplate.h"
#include "FD2D/Application.h"
#include "FD2D/Backplate.h"
#include "FD2D/Core.h"
#include "AppIpc.h"
#include "AppSetup.h"
#include "ImageBrowser.h"
#include "IpcCompareRequest.h"

#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageDecodeDispatcher.h"

#include <algorithm>
#include <cwctype>
#include <objbase.h>
#include <shellapi.h>
#include <ole2.h>
#include <vector>
#include <unordered_set>
#include <knownfolders.h>
#include <shlobj_core.h>

#define MAX_LOADSTRING 100

WCHAR g_title[MAX_LOADSTRING];

namespace
{
    static std::wstring GetInitialFileFromCommandLine()
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv || argc <= 1)
        {
            if (argv)
            {
                LocalFree(argv);
            }
            return L"";
        }

        std::wstring initial;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i] ? argv[i] : L"";
            if (arg.empty())
            {
                continue;
            }
            if (arg.rfind(L"--", 0) == 0 || arg.rfind(L"-", 0) == 0)
            {
                continue;
            }

            const DWORD attr = GetFileAttributesW(arg.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                initial = arg;
                break;
            }
        }

        LocalFree(argv);
        return initial;
    }

    static std::wstring FindFirstSupportedImageInPictures()
    {
        PWSTR pwsz = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pwsz);
        if (FAILED(hr) || pwsz == nullptr)
        {
            return L"";
        }

        std::wstring picturesDir(pwsz);
        CoTaskMemFree(pwsz);
        pwsz = nullptr;

        if (picturesDir.empty())
        {
            return L"";
        }

        // Trim trailing slash/backslash
        while (!picturesDir.empty() && (picturesDir.back() == L'\\' || picturesDir.back() == L'/'))
        {
            picturesDir.pop_back();
        }

        auto toLower = [](std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c)
            {
                return static_cast<wchar_t>(towlower(c));
            });
            return s;
        };

        std::unordered_set<std::wstring> supportedExts {};
        const std::vector<std::wstring> supportedExtList = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        supportedExts.reserve(supportedExtList.size());
        for (const auto& extRaw : supportedExtList)
        {
            supportedExts.insert(toLower(extRaw));
        }

        auto hasSupportedExt = [&](const std::wstring& fileName) -> bool
        {
            const size_t dot = fileName.find_last_of(L'.');
            if (dot == std::wstring::npos)
            {
                return false;
            }
            const std::wstring ext = toLower(fileName.substr(dot));
            return supportedExts.find(ext) != supportedExts.end();
        };

        auto fileNameOnly = [](const std::wstring& full) -> std::wstring
        {
            size_t pos = full.find_last_of(L"\\/");
            if (pos == std::wstring::npos)
            {
                return full;
            }
            return full.substr(pos + 1);
        };

        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd {};
        const std::wstring pattern = picturesDir + L"\\*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            return L"";
        }

        do
        {
            const std::wstring name = fd.cFileName;
            if (name.empty() || name == L"." || name == L"..")
            {
                continue;
            }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                continue;
            }
            if (!hasSupportedExt(name))
            {
                continue;
            }
            files.push_back(picturesDir + L"\\" + name);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);

        if (files.empty())
        {
            return L"";
        }

        std::sort(files.begin(), files.end(), [&](const std::wstring& a, const std::wstring& b)
        {
            return toLower(fileNameOnly(a)) < toLower(fileNameOnly(b));
        });

        return files.front();
    }

    static constexpr wchar_t kSingleInstanceMutex[] = L"Local\\FICture2_SingleInstance";
    static constexpr UINT WM_FIC2_IPC_COMPARE = WM_APP + 0x7A12;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    std::wstring cmdLine = (lpCmdLine != nullptr) ? lpCmdLine : L"";
    std::wstring cmdLower = cmdLine;
    std::transform(cmdLower.begin(), cmdLower.end(), cmdLower.begin(), [](wchar_t c)
    {
        return static_cast<wchar_t>(towlower(c));
    });

    // Extract initial file path (if any).
    const std::wstring initialFile = GetInitialFileFromCommandLine();

#if defined(_DEBUG)
    {
        wchar_t msg[1024] {};
        swprintf_s(msg, L"[FICture2] Startup initialFile='%s'\n", initialFile.c_str());
        OutputDebugStringW(msg);
    }
#endif

    // Single-instance detection (Named Mutex):
    // - If an instance already exists, we use IPC to ask it to start compare mode if filenames match.
    // - If filenames do not match, we keep running as a new instance.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    const DWORD mutexErr = GetLastError();
    const bool hadExistingInstance = (instanceMutex != nullptr && mutexErr == ERROR_ALREADY_EXISTS);

    if (hadExistingInstance && !initialFile.empty())
    {
        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        if (AppIpc::TrySendPath(initialFile, decision) && decision == AppIpc::Decision::CompareStarted)
        {
            // Existing instance will enter compare mode (split view); exit this process.
            return 0;
        }
        // else: server not available or decided to ignore -> continue as a new instance.
    }

    // OLE/COM lifetime is owned by the application (not FD2D).
    // OLE drag&drop (IDropTarget/RegisterDragDrop) requires OLE init on the UI thread.
    bool oleInitialized = false;
    HRESULT oleHr = OleInitialize(nullptr);
    if (SUCCEEDED(oleHr))
    {
        oleInitialized = true;
    }
    else if (oleHr == RPC_E_CHANGED_MODE)
    {
        // Another component initialized COM with a different model; continue without owning lifetime.
        oleInitialized = false;
    }
    else
    {
        return -1;
    }

    auto& app = FD2D::Application::Instance();

    // Register built-in image decoders before any folder scan or decode request
    ImageCore::RegisterBuiltInDecoders();

    FD2D::InitContext initContext {};
    initContext.instance = hInstance;
    if (FAILED(app.Initialize(initContext)))
    {
        return -1;
    }

    // First-run: ask for per-user file associations (INI existence determines first-run).
    FICture2App::RunFirstRunAssociationPromptIfNeeded();

    // Log detected Direct2D version at startup
    {
        const char* d2dVersionStr = FD2D::Core::GetD2DVersionString();
        FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
        wchar_t dbgMsg[256];
        swprintf_s(dbgMsg, L"[FICture2] Direct2D Version: %S (enum value: %d)\n",
            d2dVersionStr, static_cast<int>(d2dVersion));
        OutputDebugStringW(dbgMsg);
    }

    LoadStringW(hInstance, IDS_APP_TITLE, g_title, MAX_LOADSTRING);

    FD2D::WindowOptions opts {};
    opts.title = g_title;
    opts.chrome = FD2D::ChromeStyle::Standard;
    opts.instance = hInstance;
    opts.iconLarge = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FICTURE2));
    opts.iconSmall = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FICTURE2));
    // User override:
    //   --renderer=d2d  : force D2D-only renderer (more compatible, no D3D pass)
    //   --renderer=d3d  : prefer D3D11 swapchain renderer (default, fastest)
    if (cmdLower.find(L"--renderer=d2d") != std::wstring::npos)
    {
        opts.rendererId = L"d2d_hwndrt";
    }
    else if (cmdLower.find(L"--renderer=d3d") != std::wstring::npos)
    {
        opts.rendererId = L"d3d11_swapchain";
    }

    int result = -1;
    {
        // IMPORTANT:
        // Ensure all UI objects (Backplate/Wnd tree) are destroyed BEFORE OleUninitialize().
        // Some COM-backed objects used by the UI/render stack must be released
        // before the calling thread uninitializes COM.
        auto backplate = std::make_shared<Ficture2Backplate>(L"main");
        if (!backplate || FAILED(backplate->CreateWindowed(opts)) || !app.RegisterBackplate(backplate))
        {
            app.Shutdown();
            return FALSE;
        }

        // Persist window placement while the HWND is still valid (before destruction).
        backplate->SetOnBeforeDestroy([](HWND hwnd)
        {
            FICture2App::SaveWindowPlacement(hwnd);
            ImageBrowser_SaveSessionToIni(FICture2App::GetIniFilePath());
        });

        // Autosave window placement when the user moves/resizes the window.
        // Debounced inside Backplate to avoid excessive INI writes.
        backplate->SetOnWindowPlacementChanged([](HWND hwnd)
        {
            FICture2App::SaveWindowPlacement(hwnd);
        });

        // Core viewer: supports 1..4 panes (we start with 1 by default).
        backplate->AddWnd(CreateImageBrowser(L"viewer", initialFile));

        // Restore last window position/size (per-user INI).
        FICture2App::LoadWindowPlacement(backplate->Window());

        // Restore viewer session only when launched with no file argument.
        // (If a file is provided, we respect that and ignore saved folders/files.)
        if (initialFile.empty())
        {
            const bool restored = ImageBrowser_TryRestoreSessionFromIni(FICture2App::GetIniFilePath());
#if defined(_DEBUG)
            {
                wchar_t msg[256] {};
                swprintf_s(msg, L"[FICture2] RestoreSession result=%d\n", restored ? 1 : 0);
                OutputDebugStringW(msg);
            }
#endif

            // First run / no session: open the first supported image from the user's Pictures folder.
            if (!restored)
            {
                const std::wstring firstPicture = FindFirstSupportedImageInPictures();
                if (!firstPicture.empty())
                {
                    ImageBrowser_OpenFileInRoot(firstPicture);
                }
            }
        }

        if (!hadExistingInstance)
        {
            // First instance: start IPC server.
            // Server thread marshals requests onto the UI thread via Backplate broadcast.
            std::weak_ptr<FD2D::Backplate> weakBackplate = backplate;
            AppIpc::StartServer([weakBackplate](const std::wstring& path) -> AppIpc::Decision
            {
                auto bp = weakBackplate.lock();
                if (!bp || bp->Window() == nullptr)
                {
                    return AppIpc::Decision::Ignore;
                }

                HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (doneEvent == nullptr)
                {
                    return AppIpc::Decision::Ignore;
                }

                auto* req = new IpcCompareRequest();
                req->path = path;
                req->doneEvent = doneEvent;
                req->compareStarted = false;

                auto* bm = new FD2D::Backplate::BroadcastMessage();
                bm->message = WM_FIC2_IPC_COMPARE;
                bm->wParam = 0;
                bm->lParam = reinterpret_cast<LPARAM>(req);

                if (!PostMessageW(bp->Window(), FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
                {
                    CloseHandle(doneEvent);
                    delete req;
                    delete bm;
                    return AppIpc::Decision::Ignore;
                }

                const DWORD wait = WaitForSingleObject(doneEvent, 800);
                AppIpc::Decision decision = AppIpc::Decision::Ignore;
                if (wait == WAIT_OBJECT_0 && req->compareStarted)
                {
                    decision = AppIpc::Decision::CompareStarted;
                }

                CloseHandle(doneEvent);
                delete req;
                return decision;
            });
        }

        backplate->Show(nCmdShow);

        result = app.RunMessageLoop();

        // (Window placement + session are saved in Backplate::SetOnBeforeDestroy callback.)
        // backplate is destroyed here (end of scope), releasing any COM resources it owns.
    }

    if (instanceMutex != nullptr)
    {
        CloseHandle(instanceMutex);
        instanceMutex = nullptr;
    }

    app.Shutdown();

    if (oleInitialized)
    {
        OleUninitialize();
    }
    return result;
}

