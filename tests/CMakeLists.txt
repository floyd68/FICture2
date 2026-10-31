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
    MainImageViewTests.cpp
    MainImagePresentationStateTests.cpp
    MainImageLoadSessionTests.cpp
    MainImageTestSupport.h
    MainImageHandoffTests.cpp
    CompressedPassthroughTests.cpp
    SrgbReadbackTests.cpp
    ImageAsyncBindingTests.cpp
    FolderEnumerationTests.cpp
    ArchiveCancelTestSupport.h
    ArchiveReadCancelTests.cpp
    ArchiveHandoffTests.cpp
    PathClassificationTests.cpp
    CloudThumbnailTests.cpp
    AppShutdownTests.cpp
    AppIpcTests.cpp
    ImageBrowserPathStateTests.cpp
    ImageBrowserSplitPanesTests.cpp
    ThumbItemModelTests.cpp
    ThumbStripLayoutTests.cpp
    ThumbStripScrollTests.cpp
    ThumbStripViewportTests.cpp
    ThumbListingSessionTests.cpp
    ImageBrowserSessionPlanTests.cpp
    ThumbPayloadCacheTests.cpp
    ThumbTestSupport.h
    ThumbStripTilesTests.cpp
    ThumbSizePolicyTests.cpp
    ThumbSizeUpgradeTests.cpp
    ImageBrowserRoutingTests.cpp
    ImageBrowserThumbStripTests.cpp
    ListingRenderTests.cpp
    ListingPixelTests.cpp
    TestAssets.rc
    IpcOpenQueueTests.cpp
    ImageBrowserDeferredActionsTests.cpp
    InfoPresenterTests.cpp
    RenderSchedulingTests.cpp
    IniStoreTests.cpp
    RecentFilesStoreTests.cpp
    ViewerSettingsTests.cpp
    BackplateCommandTests.cpp
    BrowserCommandMapTests.cpp
    BrowserRegistryTests.cpp
    AppDialogsTests.cpp
    ${PROJECT_SOURCE_DIR}/AppLog.cpp
    ${PROJECT_SOURCE_DIR}/ImageGpuResourceCache.cpp
    ${PROJECT_SOURCE_DIR}/ImageAlphaPresentation.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImage.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImageView.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImagePresentation.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImageLoadSession.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbImage.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbModel.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbPayloadCache.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbSizePolicy.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbLayout.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbScroll.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbViewport.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserAssets.cpp
    ${PROJECT_SOURCE_DIR}/ThumbImageTile.cpp
    ${PROJECT_SOURCE_DIR}/ThumbNavTile.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserAsyncThumbLoader.cpp
    ${PROJECT_SOURCE_DIR}/ImageAsyncBinding.cpp
    ${PROJECT_SOURCE_DIR}/CloudThumbnailRouter.cpp
    ${PROJECT_SOURCE_DIR}/ShellThumbnailSource.cpp
    # The rest of the app (everything but FICture2.cpp, which holds WinMain)
    # for tests that build real image browsers.
    ${PROJECT_SOURCE_DIR}/AppIpc.cpp
    ${PROJECT_SOURCE_DIR}/AppSetup.cpp
    ${PROJECT_SOURCE_DIR}/AppShutdown.cpp
    ${PROJECT_SOURCE_DIR}/ArchiveEntryHandoff.cpp
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
    ${PROJECT_SOURCE_DIR}/ImageBrowserSessionPlan.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserSplitCoordinator.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbnailPane.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbLayoutCoordinator.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserThumbStripController.cpp
    ${PROJECT_SOURCE_DIR}/ImageViewUtil.cpp
    ${PROJECT_SOURCE_DIR}/RecentFiles.cpp
    ${PROJECT_SOURCE_DIR}/ViewerSettings.cpp
    ${PROJECT_SOURCE_DIR}/BrowserCommandMap.cpp
    ${PROJECT_SOURCE_DIR}/BrowserRegistry.cpp
    ${PROJECT_SOURCE_DIR}/AppDialogs.cpp
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

# Each CTest entry runs the FICture2Tests whose names start with a prefix.
# FICture2.TestGroupsRegistered (below) fails for a test no entry runs.
set(FICTURE2_TEST_PREFIXES "")
function(fic2_add_test_group name prefix)
    add_test(NAME FICture2.${name} COMMAND FICture2Tests ${prefix})
    set(FICTURE2_TEST_PREFIXES ${FICTURE2_TEST_PREFIXES} ${prefix} PARENT_SCOPE)
endfunction()

fic2_add_test_group(IpcOpenQueue IpcOpenQueue_)
fic2_add_test_group(DeferredActions DeferredActions_)
fic2_add_test_group(AppIpc AppIpc_)
fic2_add_test_group(InfoPresenter InfoPresenter_)
fic2_add_test_group(ImageAsyncBinding ImageAsyncBinding_)
fic2_add_test_group(PathState PathState_)
fic2_add_test_group(GpuPresentation GpuPresentation_)
fic2_add_test_group(MainImage MainImage_)
fic2_add_test_group(MainImageHandoff MainImageHandoff_)
fic2_add_test_group(MainImageView MainImageView_)
fic2_add_test_group(MainImagePresentationState MainImagePresentationState_)
fic2_add_test_group(MainImageLoadSession MainImageLoadSession_)
fic2_add_test_group(CompressedPassthrough CompressedPassthrough_)
fic2_add_test_group(SrgbReadback SrgbReadback_)
fic2_add_test_group(IdleRendering IdleRendering_)
fic2_add_test_group(ImageBrowserRouting ImageBrowserRouting_)
fic2_add_test_group(ImageBrowserSplitPanes ImageBrowserSplitPanes_)
fic2_add_test_group(ThumbItemModel ThumbItemModel_)
fic2_add_test_group(ThumbStripLayout ThumbStripLayout_)
fic2_add_test_group(ThumbStripScroll ThumbStripScroll_)
fic2_add_test_group(ThumbStripViewport ThumbStripViewport_)
fic2_add_test_group(ThumbListingSession ThumbListingSession_)
fic2_add_test_group(ImageBrowserSessionPlan ImageBrowserSessionPlan_)
fic2_add_test_group(ThumbPayloadCache ThumbPayloadCache_)
fic2_add_test_group(ThumbStripTiles ThumbStripTiles_)
fic2_add_test_group(ThumbSizePolicy ThumbSizePolicy_)
fic2_add_test_group(ThumbSizeUpgrade ThumbSizeUpgrade_)
fic2_add_test_group(ImageBrowserThumbStrip ImageBrowserThumbStrip_)
fic2_add_test_group(ListingRender ListingRender_)
fic2_add_test_group(ListingPixels ListingPixels_)
fic2_add_test_group(RenderScheduling RenderScheduling_)
fic2_add_test_group(IniStore IniStore_)
fic2_add_test_group(RecentFiles RecentFiles_)
fic2_add_test_group(ViewerSettings ViewerSettings_)
fic2_add_test_group(BackplateCommands BackplateCommands_)
fic2_add_test_group(BrowserCommandMap BrowserCommandMap_)
fic2_add_test_group(BrowserRegistry BrowserRegistry_)
fic2_add_test_group(AppDialogs AppDialogs_)
fic2_add_test_group(FolderEnumeration FolderEnumeration_)
fic2_add_test_group(FolderEnumerationForcedExit FolderEnumerationForcedExit_)
fic2_add_test_group(ArchiveReadCancel ArchiveReadCancel_)
fic2_add_test_group(ArchiveHandoff ArchiveHandoff_)
fic2_add_test_group(PathClassification PathClassification_)
fic2_add_test_group(CloudThumbnail CloudThumbnail_)
fic2_add_test_group(AppShutdown AppShutdown_)
fic2_add_test_group(AppShutdownForcedExit AppShutdownForcedExit_)
# AppLog::Init runs once per process, so each Init scenario gets its own entry.
fic2_add_test_group(AppLogText AppLogText_)
fic2_add_test_group(AppLogLazy AppLogLazy_)
fic2_add_test_group(AppLogInitUnicode AppLogInitUnicode_)
fic2_add_test_group(AppLogInitFailure AppLogInitFailure_)

# Every test compiled into FICture2Tests must be run by an entry above.
list(JOIN FICTURE2_TEST_PREFIXES "|" fic2_test_prefixes)
add_test(NAME FICture2.TestGroupsRegistered
    COMMAND ${CMAKE_COMMAND} -DTESTS_EXE=$<TARGET_FILE:FICture2Tests> "-DPREFIXES=${fic2_test_prefixes}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckTestGroups.cmake)
