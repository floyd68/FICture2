#include "AppShutdown.h"

#include "AppLog.h"
#include "ArchiveCursorPool.h"
#include "ArchiveEntryHandoff.h"
#include "CloudThumbnailRouter.h"
#include "FolderEnumeration.h"
#include "ImageAsyncBinding.h"
#include "ImageGpuResourceCache.h"
#include "FD2D/Application.h"
#include "ImageCore/ImageLoader.h"

#include <ole2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

const char* AppShutdownStageName(AppShutdownStage stage)
{
    switch (stage)
    {
    case AppShutdownStage::IpcServer: return "IPC server";
    case AppShutdownStage::FolderEnumeration: return "folder enumeration";
    case AppShutdownStage::ImageCore: return "ImageCore";
    case AppShutdownStage::ArchiveOpens: return "Floar archive opens";
    case AppShutdownStage::GpuCache: return "GPU resource cache";
    case AppShutdownStage::Fd2d: return "FD2D";
    case AppShutdownStage::Log: return "log";
    case AppShutdownStage::Ole: return "OLE";
    }
    return "unknown";
}

const char* AppExitPathName(AppExitPath path)
{
    switch (path)
    {
    case AppExitPath::Normal: return "normal exit";
    case AppExitPath::OleInitFailed: return "OleInitialize failed";
    case AppExitPath::Fd2dInitFailed: return "FD2D initialize failed";
    case AppExitPath::WindowCreationFailed: return "window creation failed";
    }
    return "unknown";
}

AppShutdownHooks DefaultAppShutdownHooks(
    bool oleInitialized,
    std::function<void()> beforeForcedExit,
    Floar::ArchiveReaderCache& archiveCache,
    AppIpc::Server* ipcServer)
{
    AppShutdownHooks hooks {};

    hooks.stopIpcServer = [ipcServer](std::chrono::milliseconds bound)
    {
        return ipcServer == nullptr || ipcServer->Stop(bound);
    };

    hooks.finishFolderEnumeration = [](std::chrono::milliseconds bound)
    {
        FolderEnumerationService& service = FolderEnumerationService::Shared();
        if (service.Shutdown(bound))
        {
            return true;
        }
        FIC2_LOG_WARN("[FolderEnum] {} enumeration(s) still running.", service.RunningCount());
        return false;
    };

    hooks.shutdownImageCore = [](std::chrono::milliseconds bound)
    {
        // Supersede every binding first, so no completion is applied, then stop
        // the shell thumbnail workers (whose fallbacks go to ImageCore) and
        // join ImageCore without constructing it, within one bound.
        ImageAsyncBindingRegistry::Instance().ShutdownPrepare();
        const auto start = std::chrono::steady_clock::now();
        if (!CloudThumbnailRouter::ShutdownSharedIfCreated(bound))
        {
            return false;
        }
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return ImageCore::ImageLoader::ShutdownIfCreated((std::max)(std::chrono::milliseconds { 0 }, bound - spent));
    };

    hooks.shutdownArchiveOpens = [cache = &archiveCache](std::chrono::milliseconds bound)
    {
        // ImageCore is stopped: no read can park or take entries any more.
        ArchiveEntryHandoff::Shared().Clear();
        (void)Floar::ArchiveCursorPool::Shared()->TrimIdle();
        if (cache->Shutdown(bound))
        {
            return true;
        }
        FIC2_LOG_WARN("[Floar] {} archive open(s) still running.", cache->RunningOpenCount());
        return false;
    };

    // Release cached SRVs explicitly: the process-static GPU cache must not keep
    // the graphics device alive, whichever views are still attached.
    hooks.clearGpuCache = [] { ImageGpuResourceCache::Instance().Clear(); };
    // Clears the backplates and shuts down Core; harmless when never initialized.
    hooks.shutdownFd2d = [] { FD2D::Application::Instance().Shutdown(); };
    hooks.shutdownLog = [] { AppLog::Shutdown(); };

    auto oleOwned = std::make_shared<std::atomic<bool>>(oleInitialized);
    hooks.uninitializeOle = [oleOwned]
    {
        if (oleOwned->exchange(false))
        {
            OleUninitialize();
        }
    };

    hooks.forcedExit = [beforeForcedExit = std::move(beforeForcedExit)](
        AppShutdownStage stage, std::chrono::milliseconds bound, UINT exitCode)
    {
        EndProcessAfterShutdownTimeout(AppShutdownStageName(stage), bound, exitCode, beforeForcedExit);
    };

    return hooks;
}

namespace
{
    void Run(const std::function<void()>& hook)
    {
        if (hook)
        {
            hook();
        }
    }

    // False when the stage timed out (the forced-exit hook then returned).
    bool RunBounded(
        const AppShutdownHooks& hooks,
        AppShutdownStage stage,
        const std::function<bool(std::chrono::milliseconds)>& hook,
        std::chrono::milliseconds bound,
        UINT exitCode)
    {
        if (!hook || hook(bound))
        {
            return true;
        }
        if (hooks.forcedExit)
        {
            hooks.forcedExit(stage, bound, exitCode);
        }
        else
        {
            EndProcessAfterShutdownTimeout(AppShutdownStageName(stage), bound, exitCode, {});
        }
        return false;
    }
}

AppShutdownResult RunAppShutdown(
    AppExitPath path,
    const AppShutdownHooks& hooks,
    const AppShutdownBounds& bounds)
{
    FIC2_LOG_INFO("=== FICture2 shutdown ({}) ===", AppExitPathName(path));

    // No request arrives once the IPC server stopped. Every archive-cache
    // caller stops before the cache's open workers.
    if (!RunBounded(hooks, AppShutdownStage::IpcServer, hooks.stopIpcServer,
            bounds.ipcServer, kIpcServerForcedExitCode) ||
        !RunBounded(hooks, AppShutdownStage::FolderEnumeration, hooks.finishFolderEnumeration,
            bounds.folderEnumeration, kFolderEnumerationForcedExitCode) ||
        !RunBounded(hooks, AppShutdownStage::ImageCore, hooks.shutdownImageCore,
            bounds.imageCore, kImageCoreForcedExitCode) ||
        !RunBounded(hooks, AppShutdownStage::ArchiveOpens, hooks.shutdownArchiveOpens,
            bounds.archiveOpens, kArchiveOpenForcedExitCode))
    {
        return AppShutdownResult::ForcedExit;
    }

    Run(hooks.clearGpuCache);
    Run(hooks.shutdownFd2d);
    Run(hooks.shutdownLog);
    Run(hooks.uninitializeOle);
    return AppShutdownResult::Completed;
}
