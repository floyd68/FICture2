#pragma once

// Post-UI cleanup of the application's worker-owning services, shared by the
// normal exit and every startup failure path of wWinMain (see the shutdown
// notes in FolderEnumeration.h).
//
// Order, one stage each:
//   1. IPC server, bounded: the stop event wakes the accept wait and every
//      client read/write, then the accept and client threads are joined
//      (AppIpc::Server::Stop), so none of them can log or touch the app
//      after the log and static teardown;
//   2. folder enumeration, bounded;
//   3. ImageCore, bounded: binding invalidation, then the shell thumbnail
//      workers of online-only cloud files (CloudThumbnailRouter, none started
//      unless one was routed), then the non-constructing
//      ImageLoader::ShutdownIfCreated with the rest of the bound, which
//      requests stop on every request token and joins ImageCore's threads (it
//      creates no loader, so a path that never used ImageCore starts no decode
//      worker);
//   4. Floar shared-open service, bounded: the solid-archive handoff's parked
//      bytes are dropped and the idle forward cursors closed, then
//      ArchiveReaderCache::Shutdown(bound) joins the archive open workers
//      (none start if no open ran);
//   5. GPU resource cache clear, FD2D shutdown, log shutdown, OLE uninitialize.
// Every archive-cache caller (enumeration, ImageCore) stops before the cache,
// so no request decodes, falls back to a path read or publishes a completion
// because the cache was shut down.
//
// Bounded stages: when the stage's workers joined within its bound the cleanup
// continues; otherwise it calls the forced-exit hook with the stage and its
// exit code (FolderEnumeration.h) and runs no later stage. The real hook
// (EndProcessAfterShutdownTimeout) ends the process, so normal and static
// teardown never run concurrently with a stuck worker.
//
// Repeatable and safe before the services started: every real stage tolerates
// a service that never ran or already shut down, and OLE is uninitialized at
// most once per hook set. The cleanup never persists the session or the window
// placement; that stays in the window's before-destroy callback.

#include "AppIpc.h"
#include "ArchiveReaderCache.h"

#include <Windows.h>

#include <chrono>
#include <functional>

// Declared bounds of the bounded stages.
constexpr std::chrono::milliseconds kIpcServerShutdownBound { 1000 };
constexpr std::chrono::milliseconds kFolderEnumerationShutdownBound { 2000 };
constexpr std::chrono::milliseconds kImageCoreShutdownBound { 2000 };
constexpr std::chrono::milliseconds kArchiveOpenShutdownBound { 2000 };

enum class AppShutdownStage
{
    IpcServer,
    FolderEnumeration,
    ImageCore,
    ArchiveOpens,
    GpuCache,
    Fd2d,
    Log,
    Ole,
};

const char* AppShutdownStageName(AppShutdownStage stage);

// Which wWinMain path runs the cleanup (logged; the policy is the same).
enum class AppExitPath
{
    Normal,
    OleInitFailed,
    Fd2dInitFailed,
    WindowCreationFailed,
};

const char* AppExitPathName(AppExitPath path);

// The stages, injectable for tests. An empty hook is skipped (a bounded one
// counts as joined).
struct AppShutdownHooks
{
    // Bounded stages: true when every worker joined within the bound.
    std::function<bool(std::chrono::milliseconds bound)> stopIpcServer {};
    std::function<bool(std::chrono::milliseconds bound)> finishFolderEnumeration {};
    // Binding invalidation plus the ImageCore shutdown.
    std::function<bool(std::chrono::milliseconds bound)> shutdownImageCore {};
    std::function<bool(std::chrono::milliseconds bound)> shutdownArchiveOpens {};
    std::function<void()> clearGpuCache {};
    std::function<void()> shutdownFd2d {};
    std::function<void()> shutdownLog {};
    std::function<void()> uninitializeOle {};
    // Called for the first bounded stage that timed out. The real hook does not
    // return; a test hook that returns makes the cleanup return
    // AppShutdownResult::ForcedExit without running a later stage. When empty,
    // EndProcessAfterShutdownTimeout runs.
    std::function<void(AppShutdownStage stage, std::chrono::milliseconds bound, UINT exitCode)> forcedExit {};
};

struct AppShutdownBounds
{
    std::chrono::milliseconds ipcServer { kIpcServerShutdownBound };
    std::chrono::milliseconds folderEnumeration { kFolderEnumerationShutdownBound };
    std::chrono::milliseconds imageCore { kImageCoreShutdownBound };
    std::chrono::milliseconds archiveOpens { kArchiveOpenShutdownBound };
};

// The real stages: `ipcServer` (none when null, as in a second instance or
// before the server started), FolderEnumerationService::Shared(), ImageAsyncBindingRegistry
// plus CloudThumbnailRouter::ShutdownSharedIfCreated and
// ImageLoader::ShutdownIfCreated, `archiveCache`, ImageGpuResourceCache,
// FD2D::Application, AppLog, OleUninitialize (only when `oleInitialized`, and
// at most once) and EndProcessAfterShutdownTimeout with `beforeForcedExit`.
AppShutdownHooks DefaultAppShutdownHooks(
    bool oleInitialized,
    std::function<void()> beforeForcedExit = {},
    Floar::ArchiveReaderCache& archiveCache = Floar::ArchiveReaderCache::Shared(),
    AppIpc::Server* ipcServer = nullptr);

enum class AppShutdownResult
{
    Completed,
    ForcedExit,   // only when a test's forced-exit hook returned
};

AppShutdownResult RunAppShutdown(
    AppExitPath path,
    const AppShutdownHooks& hooks,
    const AppShutdownBounds& bounds = {});
