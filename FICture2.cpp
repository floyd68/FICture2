// FICture2.cpp : application entrypoint

#include "FICture2.h"

#include "Ficture2Backplate.h"
#include "FD2D/Application.h"
#include "FD2D/Backplate.h"
#include "FD2D/Core.h"
#include "FD2D/FD2DLog.h"
#include "AppIpc.h"
#include "AppLog.h"
#include "AppSetup.h"
#include "CommonUtil.h"
#include "FloarPathByteSource.h"
#include "ImageBrowser.h"
#include "IpcOpenRequest.h"

#include "FloarLog.h"
#include "ImageAsyncBinding.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageCoreLog.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "ImageCore/ImageLoader.h"
#include "ImageCore/IPathByteSource.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <memory>
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
    // Standalone library/submodules no longer depend on AppLog.h directly;
    // forward their sink-based logs into the same "fic2" spdlog logger.
    template <typename LevelT>
    static void ForwardLibraryLogToAppLog(LevelT level, const std::string& message)
    {
        auto logger = spdlog::get("fic2");
        if (!logger)
        {
            return;
        }
        using L = LevelT;
        switch (level)
        {
        case L::Trace: logger->trace(message); break;
        case L::Debug: logger->debug(message); break;
        case L::Info:  logger->info(message);  break;
        case L::Warn:  logger->warn(message);  break;
        case L::Error: logger->error(message); break;
        }
    }

    static void ForwardFD2DLogToAppLog(FD2D::Log::Level level, const std::string& message)
    {
        ForwardLibraryLogToAppLog(level, message);
    }

    static void ForwardFloarLogToAppLog(Floar::Log::Level level, const std::string& message)
    {
        ForwardLibraryLogToAppLog(level, message);
    }

    static void ForwardImageCoreLogToAppLog(ImageCore::Log::Level level, const std::string& message)
    {
        ForwardLibraryLogToAppLog(level, message);
    }

    static FloarPathByteSource& GetFloarPathByteSource()
    {
        static FloarPathByteSource s_source {};
        return s_source;
    }

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

    // Sentinel HWND published through ipcUiWindow once shutdown begins.
    const HWND kIpcUiWindowGone = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));

    constexpr std::size_t kMaxIpcPanes = 4;
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

    // Install Floar-backed byte source for ImageCore decode/prefetch (archives + disk).
    ImageCore::SetPathByteSource(&GetFloarPathByteSource());

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
            FD2D::Log::SetSink(&ForwardFD2DLogToAppLog);
            Floar::Log::SetSink(&ForwardFloarLogToAppLog);
            ImageCore::Log::SetSink(&ForwardImageCoreLogToAppLog);

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

        // Let the running instance take foreground when it accepts the file -
        // this fresh process currently holds that right.
        AllowSetForegroundWindow(ASFW_ANY);

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

    // Primary instance: start the IPC server IMMEDIATELY. A second instance's
    // pipe-connect budget is 500ms from *its* launch, while this instance still
    // has OleInitialize / FD2D device creation / session restore ahead of it.
    //
    // The callback (per-client AppIpc worker) decides on the spot against the
    // shared IpcOpenQueue - same-file-name gate + pane capacity - queues the
    // accepted path, and answers immediately. The UI drains the queue when
    // CMD_FIC2_IPC_OPEN arrives (or right after startup). It never waits for
    // the UI to load anything.
    auto ipcUiWindow = std::make_shared<std::atomic<HWND>>(nullptr);
    auto ipcQueue = std::make_shared<IpcOpenQueue>();
    if (!hadExistingInstance)
    {
        FIC2_LOG_INFO("[IPC] Server: starting named-pipe server (pre-UI init).");
        if (!initialFile.empty())
        {
            ipcQueue->SeedExpected({ initialFile });
        }
        AppIpc::StartServer([ipcUiWindow, ipcQueue](const std::wstring& path) -> AppIpc::Decision
        {
            FIC2_LOG_INFO("[IPC] Server: incoming request for path: {}",
                std::filesystem::path(path).string());

            if (!ipcQueue->TryEnqueue(path, kMaxIpcPanes))
            {
                FIC2_LOG_INFO("[IPC] Server: declined (no name match / capacity / not ready).");
                return AppIpc::Decision::Ignore;
            }

            HWND hwnd = ipcUiWindow->load();
            if (hwnd != nullptr && hwnd != kIpcUiWindowGone)
            {
                auto* bm = new FD2D::Backplate::BroadcastMessage();
                bm->message = CMD_FIC2_IPC_OPEN;
                bm->wParam = 0;
                bm->lParam = 0;
                if (!PostMessageW(hwnd, FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
                {
                    delete bm; // queue entry stays; startup/next drain picks it up
                }
                if (IsIconic(hwnd))
                {
                    ShowWindowAsync(hwnd, SW_RESTORE);
                }
                SetForegroundWindow(hwnd);
            }
            FIC2_LOG_INFO("[IPC] Server: queued path (CompareStarted).");
            return AppIpc::Decision::CompareStarted;
        });
    }

    // --- Startup timing baseline (from just after IPC client check / StartServer) ---
    FIC2_TIMER_START(t_startup);
    if (!hadExistingInstance)
    {
        FIC2_LOG_STEP(t_startup, "StartServer (pipe now available)");
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
            ImageAsyncBindingRegistry::Instance().ShutdownPrepare();
            ImageCore::ImageLoader::Instance().Shutdown();
            app.Shutdown();
            return FALSE;
        }
        FIC2_LOG_STEP(t_startup, "Ficture2Backplate::CreateWindowed (HWND + render device)");

        // Force the first D3D/D2D render pass to absorb the GPU driver cold-start
        // penalty (shader compilation, pipeline state caching — typically 150–200 ms).
        // The IPC pipe is already listening; queue-or-decline workers answer without
        // waiting for this warm-up. The window is not yet visible so there is no
        // visual artifact.
        backplate->Render();
        FIC2_LOG_STEP(t_startup, "Render() warm-up (GPU driver cold start)");

        backplate->SetIpcOpenQueue(ipcQueue);

        // Persist window placement while the HWND is still valid (before destruction).
        backplate->SetOnBeforeDestroy([iniFile, weakBackplate = std::weak_ptr<Ficture2Backplate>(backplate), ipcUiWindow, ipcQueue](HWND hwnd)
        {
            ipcQueue->MarkShuttingDown();
            // Stop routing IPC wakes at a window that is about to die.
            ipcUiWindow->store(kIpcUiWindowGone);
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

        // UI is fully wired (browser attached, session restored) - publish the
        // window so queued IPC opens can wake the UI thread, then drain anything
        // accepted during boot.
        ipcQueue->MarkUiReady();
        backplate->RefreshIpcOpenSnapshot();
        ipcUiWindow->store(backplate->Window());
        backplate->DrainIpcOpenQueue();

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

    // Tear down async image bindings and ImageCore before FD2D::Application::Shutdown
    // (which clears backplates and shuts down Core).
    ImageAsyncBindingRegistry::Instance().ShutdownPrepare();
    ImageCore::ImageLoader::Instance().Shutdown();

    app.Shutdown();

    FIC2_LOG_INFO("=== FICture2 shutdown (result={}) ===", result);
    AppLog::Shutdown();

    if (oleInitialized)
    {
        OleUninitialize();
    }
    return result;
}

