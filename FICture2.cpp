// FICture2.cpp : application entrypoint

#include "FICture2.h"

#include "Ficture2Backplate.h"
#include "FD2D/Application.h"
#include "FD2D/Backplate.h"
#include "FD2D/Core.h"
#include "AppIpc.h"
#include "AppLog.h"
#include "AppSetup.h"
#include "CommonUtil.h"
#include "ImageBrowser.h"
#include "IpcCompareRequest.h"

#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageDecodeDispatcher.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
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

        std::unordered_set<std::wstring> supportedExts {};
        const std::vector<std::wstring> supportedExtList = ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
        supportedExts.reserve(supportedExtList.size());
        for (const auto& extRaw : supportedExtList)
        {
            supportedExts.insert(CommonUtil::ToLower(extRaw));
        }

        auto hasSupportedExt = [&](const std::wstring& fileName) -> bool
        {
            const size_t dot = fileName.find_last_of(L'.');
            if (dot == std::wstring::npos)
            {
                return false;
            }
            const std::wstring ext = CommonUtil::ToLower(fileName.substr(dot));
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
            return CommonUtil::ToLower(fileNameOnly(a)) < CommonUtil::ToLower(fileNameOnly(b));
        });

        return files.front();
    }

    static constexpr wchar_t kSingleInstanceMutex[] = L"Local\\FICture2_SingleInstance";
    static constexpr UINT CMD_FIC2_IPC_COMPARE = WM_APP + 0x7A12;
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

    {
        // Logging is opt-in for Release (-logon) and opt-out for Debug (-logoff).
        // If Init is never called, spdlog::get("fic2") returns null and every
        // FIC2_LOG_* macro becomes a silent no-op — no overhead at all.
#ifdef _DEBUG
        const bool enableLog = (cmdLower.find(L"-logoff") == std::wstring::npos);
#else
        const bool enableLog = (cmdLower.find(L"-logon") != std::wstring::npos);
#endif

        if (enableLog)
        {
            wchar_t exePath[MAX_PATH] {};
            GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            AppLog::Init(exeDir.wstring());

            FIC2_LOG_INFO("=== FICture2 startup ===");
            FIC2_LOG_INFO("Executable : {}", std::filesystem::path(exePath).string());
            FIC2_LOG_INFO("CommandLine: {}", std::filesystem::path(initialFile).string());
        }
    }

    // Single-instance detection (Named Mutex):
    // - If an instance already exists, we use IPC to ask it to start compare mode if filenames match.
    // - If filenames do not match, we keep running as a new instance.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    const DWORD mutexErr = GetLastError();
    const bool hadExistingInstance = (instanceMutex != nullptr && mutexErr == ERROR_ALREADY_EXISTS);

    FIC2_LOG_INFO("[IPC] Mutex: hadExistingInstance={} (mutexErr={})",
        hadExistingInstance, mutexErr);

    if (hadExistingInstance && !initialFile.empty())
    {
        FIC2_LOG_INFO("[IPC] Client: sending path to existing instance: {}",
            std::filesystem::path(initialFile).string());

        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        const bool sent = AppIpc::TrySendPath(initialFile, decision);
        FIC2_LOG_INFO("[IPC] Client: TrySendPath result={} decision={}",
            sent, static_cast<int>(decision));

        if (sent && decision == AppIpc::Decision::CompareStarted)
        {
            // Existing instance will enter compare mode (split view); exit this process.
            FIC2_LOG_INFO("[IPC] Client: compare started in existing instance — exiting.");
            AppLog::Shutdown();
            return 0;
        }
        FIC2_LOG_WARN("[IPC] Client: server not available or chose Ignore — starting as new instance.");
        // else: server not available or decided to ignore -> continue as a new instance.
    }

    // --- Startup timing baseline (from just after IPC client check) ---
    FIC2_TIMER_START(t_startup);

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
    FIC2_LOG_STEP(t_startup, "OleInitialize");

    auto& app = FD2D::Application::Instance();

    // Register built-in image decoders before any folder scan or decode request
    ImageCore::RegisterBuiltInDecoders();
    FIC2_LOG_STEP(t_startup, "RegisterBuiltInDecoders");

    FD2D::InitContext initContext {};
    initContext.instance = hInstance;
    if (FAILED(app.Initialize(initContext)))
    {
        return -1;
    }
    FIC2_LOG_STEP(t_startup, "FD2D::Application::Initialize (D3D/D2D device creation)");

    // First-run: ask for per-user file associations (INI existence determines first-run).
    FICture2App::RunFirstRunAssociationPromptIfNeeded();
    FIC2_LOG_STEP(t_startup, "RunFirstRunAssociationPromptIfNeeded");

    // Log detected Direct2D version at startup
    {
        const char* d2dVersionStr = FD2D::Core::GetD2DVersionString();
        FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
        FIC2_LOG_INFO("Direct2D version: {} (enum={})", d2dVersionStr, static_cast<int>(d2dVersion));
    }

    LoadStringW(hInstance, IDS_APP_TITLE, g_title, MAX_LOADSTRING);

    // Read the saved window placement BEFORE building opts / calling CreateWindowed.
    // This lets us pass the saved rect directly to CreateWindowEx, which avoids a
    // subsequent SetWindowPlacement() call and its ~50 ms DWM IPC round-trip cost.
    RECT savedRect {};
    int  savedShowCmd = 0;
    const bool hasSavedPlacement = FICture2App::ReadWindowPlacement(savedRect, savedShowCmd);
    FIC2_LOG_STEP(t_startup, "ReadWindowPlacement (pre-CreateWindowed)");

    FD2D::WindowOptions opts {};
    opts.title = g_title;
    opts.chrome = FD2D::ChromeStyle::Standard;
    opts.instance = hInstance;
    opts.iconLarge = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FICTURE2));
    opts.iconSmall = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FICTURE2));
    if (hasSavedPlacement)
    {
        opts.x      = savedRect.left;
        opts.y      = savedRect.top;
        opts.width  = static_cast<UINT>(savedRect.right - savedRect.left);
        opts.height = static_cast<UINT>(savedRect.bottom - savedRect.top);
    }
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
        const std::wstring iniFile = FICture2App::GetIniFilePath();

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
        FIC2_LOG_STEP(t_startup, "Ficture2Backplate::CreateWindowed (HWND + render device)");

        // -----------------------------------------------------------------------
        // Start the IPC server IMMEDIATELY after the window is created, before
        // any further initialization that would delay the pipe becoming available.
        // The server callback posts a message to the HWND queue; the UI thread
        // will process it once it enters RunMessageLoop (after AddWnd / session
        // restore), which is within the 800ms WaitForSingleObject budget.
        // -----------------------------------------------------------------------
        if (!hadExistingInstance)
        {
            FIC2_LOG_INFO("[IPC] Server: starting named-pipe server (HWND ready, pre-AddWnd).");
            std::weak_ptr<FD2D::Backplate> weakBackplate = backplate;
            AppIpc::StartServer([weakBackplate](const std::wstring& path) -> AppIpc::Decision
            {
                FIC2_LOG_INFO("[IPC] Server: incoming request for path: {}",
                    std::filesystem::path(path).string());

                auto bp = weakBackplate.lock();
                if (!bp || bp->Window() == nullptr)
                {
                    FIC2_LOG_WARN("[IPC] Server: backplate expired or window gone — ignoring.");
                    return AppIpc::Decision::Ignore;
                }

                HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (doneEvent == nullptr)
                {
                    FIC2_LOG_ERROR("[IPC] Server: CreateEventW failed (err={}) — ignoring.", GetLastError());
                    return AppIpc::Decision::Ignore;
                }

                auto* req = new IpcCompareRequest();
                req->path = path;
                req->doneEvent = doneEvent;
                req->compareStarted = false;

                auto* bm = new FD2D::Backplate::BroadcastMessage();
                bm->message = CMD_FIC2_IPC_COMPARE;
                bm->wParam = 0;
                bm->lParam = reinterpret_cast<LPARAM>(req);

                if (!PostMessageW(bp->Window(), FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
                {
                    FIC2_LOG_ERROR("[IPC] Server: PostMessageW failed (err={}) — ignoring.", GetLastError());
                    CloseHandle(doneEvent);
                    delete req;
                    delete bm;
                    return AppIpc::Decision::Ignore;
                }

                FIC2_LOG_DEBUG("[IPC] Server: posted CMD_FIC2_IPC_COMPARE, waiting up to 800ms for UI thread.");
                const DWORD wait = WaitForSingleObject(doneEvent, 800);
                AppIpc::Decision decision = AppIpc::Decision::Ignore;
                if (wait == WAIT_OBJECT_0 && req->compareStarted)
                {
                    decision = AppIpc::Decision::CompareStarted;
                    FIC2_LOG_INFO("[IPC] Server: UI thread confirmed compare started.");
                }
                else if (wait == WAIT_TIMEOUT)
                {
                    FIC2_LOG_WARN("[IPC] Server: UI thread did not respond within 800ms — returning Ignore.");
                }
                else
                {
                    FIC2_LOG_INFO("[IPC] Server: UI thread responded (compareStarted={}).", req->compareStarted);
                }

                CloseHandle(doneEvent);
                delete req;
                FIC2_LOG_INFO("[IPC] Server: returning decision={}.", static_cast<int>(decision));
                return decision;
            });
            FIC2_LOG_STEP(t_startup, "StartServer (pipe now available)");
        }

        // Force the first D3D/D2D render pass to absorb the GPU driver cold-start
        // penalty (shader compilation, pipeline state caching — typically 150–200 ms).
        // This must come AFTER StartServer so the IPC pipe is already listening when
        // Instance 2 arrives; the warm-up blocks the UI thread but the IPC server
        // thread continues to accept the connection and queue the message, which will
        // be processed in RunMessageLoop well within the 800 ms budget.
        // The window is not yet visible so there is no visual artifact.
        backplate->Render();
        FIC2_LOG_STEP(t_startup, "Render() warm-up (GPU driver cold start)");

        // Persist window placement while the HWND is still valid (before destruction).
        backplate->SetOnBeforeDestroy([iniFile, weakBackplate = std::weak_ptr<Ficture2Backplate>(backplate)](HWND hwnd)
        {
            FICture2App::SaveWindowPlacement(hwnd);
            if (auto bp = weakBackplate.lock())
            {
                bp->SaveImageBrowserSession(iniFile);
            }
        });

        // Autosave window placement when the user moves/resizes the window.
        // Debounced inside Backplate to avoid excessive INI writes.
        backplate->SetOnWindowPlacementChanged([](HWND hwnd)
        {
            FICture2App::SaveWindowPlacement(hwnd);
        });

        // Pre-load INI settings before AddWnd so that OnAttached's
        // EnsureImageBrowserIniInitialized returns instantly (guard).
        // This also warms the disk cache for LoadWindowPlacement's file read.
        backplate->EnsureImageBrowserIniInitialized();
        FIC2_LOG_STEP(t_startup, "EnsureImageBrowserIniInitialized (pre-AddWnd)");

        // Core viewer: supports 1..4 panes (we start with 1 by default).
        auto firstBrowser = CreateImageBrowser(L"viewer", initialFile);
        FIC2_LOG_STEP(t_startup, "CreateImageBrowser (ctor + BuildUi)");
        backplate->AddWnd(firstBrowser);
        FIC2_LOG_STEP(t_startup, "backplate->AddWnd (total)");

        // Restore viewer session only when launched with no file argument.
        // (If a file is provided, we respect that and ignore saved folders/files.)
        if (initialFile.empty())
        {
            FIC2_LOG_INFO("No initial file — attempting session restore from INI.");
            const bool restored = backplate->TryRestoreImageBrowserSession(iniFile);
            FIC2_LOG_STEP(t_startup, "TryRestoreImageBrowserSession");
            FIC2_LOG_INFO("Session restore result: {}", restored ? "restored" : "not restored");

            // First run / no session: open the first supported image from the user's Pictures folder.
            if (!restored)
            {
                const std::wstring firstPicture = FindFirstSupportedImageInPictures();
                FIC2_LOG_STEP(t_startup, "FindFirstSupportedImageInPictures");
                if (!firstPicture.empty())
                {
                    FIC2_LOG_INFO("First-run: opening first picture from Pictures folder: {}",
                        std::filesystem::path(firstPicture).string());
                    backplate->OpenFileInRoot(firstPicture);
                }
            }
        }
        else
        {
            FIC2_LOG_STEP(t_startup, "(initial file provided — no session restore)");
        }

        FIC2_LOG_STEP(t_startup, "total pre-Show");
        // Use the show command saved in the INI if available; otherwise fall back
        // to the system nCmdShow (e.g. SW_SHOWNORMAL for a normal launch).
        const int effectiveShowCmd = (hasSavedPlacement && savedShowCmd != 0)
                                   ? savedShowCmd : nCmdShow;
        backplate->Show(effectiveShowCmd);

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

    FIC2_LOG_INFO("=== FICture2 shutdown (result={}) ===", result);
    AppLog::Shutdown();

    if (oleInitialized)
    {
        OleUninitialize();
    }
    return result;
}

