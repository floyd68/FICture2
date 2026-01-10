// FICture2.cpp : application entrypoint

#include "framework.h"
#include "FICture2.h"

#include "FD2D/FD2D.h"
#include "AppSetup.h"
#include "ImageBrowser.h"

#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"

#include <algorithm>
#include <memory>
#include <objbase.h>

#define MAX_LOADSTRING 100

WCHAR g_title[MAX_LOADSTRING];

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

    // COM lifetime is owned by the application (not FD2D)
    bool coInitialized = false;
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(coHr))
    {
        coInitialized = true;
    }
    else if (coHr == RPC_E_CHANGED_MODE)
    {
        // Another component initialized COM with a different model; continue without owning lifetime
        coInitialized = false;
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
        // Ensure all UI objects (Backplate/Wnd tree) are destroyed BEFORE CoUninitialize().
        // Some COM-backed objects (e.g. WIC bitmaps created for thumbnails) must be released
        // before the calling thread uninitializes COM.
        auto backplate = app.CreateWindowedBackplate(L"main", opts);
        if (!backplate)
        {
            app.Shutdown();
            return FALSE;
        }

        // Core viewer: supports 1..4 panes (we start with 1 by default).
        backplate->AddWnd(CreateImageBrowser(L"viewer", 1));

        backplate->Show(nCmdShow);

        result = app.RunMessageLoop();
        // backplate is destroyed here (end of scope), releasing any COM resources it owns.
    }

    app.Shutdown();

    if (coInitialized)
    {
        CoUninitialize();
    }
    return result;
}

