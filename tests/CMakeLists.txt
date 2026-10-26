# FICture2 first-party unit tests (enabled with -DFICTURE2_BUILD_TESTS=ON).
# Non-window tests only; vendored dependency tests stay disabled.

add_executable(FICture2Tests
    TestHarness.h
    TestMain.cpp
    BackplateTestAccess.h
    AppLogTests.cpp
    GpuPresentationTests.cpp
    IdleRenderingTests.cpp
    MainImagePresentationTests.cpp
    MainImageHandoffTests.cpp
    CompressedPassthroughTests.cpp
    ImageAsyncBindingTests.cpp
    FolderEnumerationTests.cpp
    ArchiveCancelTestSupport.h
    ArchiveReadCancelTests.cpp
    AppShutdownTests.cpp
    ImageBrowserPathStateTests.cpp
    ThumbItemModelTests.cpp
    ThumbStripLayoutTests.cpp
    ThumbPayloadCacheTests.cpp
    ThumbTestSupport.h
    ThumbStripTilesTests.cpp
    ImageBrowserRoutingTests.cpp
    ImageBrowserThumbStripTests.cpp
    ListingRenderTests.cpp
    ListingPixelTests.cpp
    TestAssets.rc
    IpcOpenQueueTests.cpp
    InfoPresenterTests.cpp
    ${PROJECT_SOURCE_DIR}/AppLog.cpp
    ${PROJECT_SOURCE_DIR}/ImageGpuResourceCache.cpp
    ${PROJECT_SOURCE_DIR}/ImageAlphaPresentation.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImage.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbImage.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbModel.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbPayloadCache.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbLayout.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserAssets.cpp
    ${PROJECT_SOURCE_DIR}/ThumbImageTile.cpp
    ${PROJECT_SOURCE_DIR}/ThumbNavTile.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserAsyncThumbLoader.cpp
    ${PROJECT_SOURCE_DIR}/ImageAsyncBinding.cpp
    # The rest of the app (everything but FICture2.cpp, which holds WinMain)
    # for tests that build real image browsers.
    ${PROJECT_SOURCE_DIR}/AppIpc.cpp
    ${PROJECT_SOURCE_DIR}/AppSetup.cpp
    ${PROJECT_SOURCE_DIR}/AppShutdown.cpp
    ${PROJECT_SOURCE_DIR}/Ficture2Backplate.cpp
    ${PROJECT_SOURCE_DIR}/FloarPathByteSource.cpp
    ${PROJECT_SOURCE_DIR}/FolderEnumeration.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowser.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserContextMenu.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserDeferredActions.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserDragController.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserInfoPresenter.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserKeyboardController.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainPane.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserSessionPersistence.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserSplitCoordinator.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbnailPane.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbLayoutCoordinator.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbStripController.cpp
    ${PROJECT_SOURCE_DIR}/ImageViewUtil.cpp
    ${PROJECT_SOURCE_DIR}/RecentFiles.cpp
    ${PROJECT_SOURCE_DIR}/ScreenshotUtil.cpp
    # The app imports Common Controls v6 entry points, which load only with its manifest.
    ${PROJECT_SOURCE_DIR}/app.manifest
)

target_include_directories(FICture2Tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/FD2D
    ${PROJECT_SOURCE_DIR}/ImageCore
    ${PROJECT_BINARY_DIR}   # generated Version.h
    ${PROJECT_SOURCE_DIR}/external/spdlog/include
    # Floar's ZIP fixture writer (TestArchives.h) and the libarchive writer it uses.
    ${PROJECT_SOURCE_DIR}/Floar/tests
    ${PROJECT_SOURCE_DIR}/Floar/external/libarchive/libarchive
)

target_compile_definitions(FICture2Tests PRIVATE
    FD2D_STATIC
    SPDLOG_WCHAR_FILENAMES
    UNICODE
    _UNICODE
    LIBARCHIVE_STATIC
    # The app's build-flavor and feature switches, for the app sources above.
    $<TARGET_PROPERTY:FICture2,COMPILE_DEFINITIONS>
)

target_link_libraries(FICture2Tests PRIVATE
    FD2D
    ImageCore
    Floar
    DirectXTex
    comctl32
    d2d1
    dwrite
    shell32
    windowscodecs
    d3d11
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_options(FICture2Tests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME FICture2.IpcOpenQueue COMMAND FICture2Tests IpcOpenQueue_)
add_test(NAME FICture2.InfoPresenter COMMAND FICture2Tests InfoPresenter_)
add_test(NAME FICture2.ImageAsyncBinding COMMAND FICture2Tests ImageAsyncBinding_)
add_test(NAME FICture2.PathState COMMAND FICture2Tests PathState_)
add_test(NAME FICture2.GpuPresentation COMMAND FICture2Tests GpuPresentation_)
add_test(NAME FICture2.MainImage COMMAND FICture2Tests MainImage_)
add_test(NAME FICture2.MainImageHandoff COMMAND FICture2Tests MainImageHandoff_)
add_test(NAME FICture2.CompressedPassthrough COMMAND FICture2Tests CompressedPassthrough_)
add_test(NAME FICture2.IdleRendering COMMAND FICture2Tests IdleRendering_)
add_test(NAME FICture2.ImageBrowserRouting COMMAND FICture2Tests ImageBrowserRouting_)
add_test(NAME FICture2.ThumbItemModel COMMAND FICture2Tests ThumbItemModel_)
add_test(NAME FICture2.ThumbStripLayout COMMAND FICture2Tests ThumbStripLayout_)
add_test(NAME FICture2.ThumbPayloadCache COMMAND FICture2Tests ThumbPayloadCache_)
add_test(NAME FICture2.ThumbStripTiles COMMAND FICture2Tests ThumbStripTiles_)
add_test(NAME FICture2.ImageBrowserThumbStrip COMMAND FICture2Tests ImageBrowserThumbStrip_)
add_test(NAME FICture2.ListingRender COMMAND FICture2Tests ListingRender_)
add_test(NAME FICture2.ListingPixels COMMAND FICture2Tests ListingPixels_)
add_test(NAME FICture2.FolderEnumeration COMMAND FICture2Tests FolderEnumeration_)
add_test(NAME FICture2.FolderEnumerationForcedExit COMMAND FICture2Tests FolderEnumerationForcedExit_)
add_test(NAME FICture2.ArchiveReadCancel COMMAND FICture2Tests ArchiveReadCancel_)
add_test(NAME FICture2.AppShutdown COMMAND FICture2Tests AppShutdown_)
add_test(NAME FICture2.AppShutdownForcedExit COMMAND FICture2Tests AppShutdownForcedExit_)
# AppLog::Init runs once per process, so each Init scenario gets its own entry.
add_test(NAME FICture2.AppLogText COMMAND FICture2Tests AppLogText_)
add_test(NAME FICture2.AppLogLazy COMMAND FICture2Tests AppLogLazy_)
add_test(NAME FICture2.AppLogInitUnicode COMMAND FICture2Tests AppLogInitUnicode_)
add_test(NAME FICture2.AppLogInitFailure COMMAND FICture2Tests AppLogInitFailure_)
