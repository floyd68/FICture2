# FICture2 first-party unit tests (enabled with -DFICTURE2_BUILD_TESTS=ON).
# Non-window tests only; vendored dependency tests stay disabled.

add_executable(FICture2Tests
    TestHarness.h
    TestMain.cpp
    AppLogTests.cpp
    GpuPresentationTests.cpp
    MainImagePresentationTests.cpp
    ImageAsyncBindingTests.cpp
    ImageBrowserPathStateTests.cpp
    IpcOpenQueueTests.cpp
    ${PROJECT_SOURCE_DIR}/AppLog.cpp
    ${PROJECT_SOURCE_DIR}/ImageGpuResourceCache.cpp
    ${PROJECT_SOURCE_DIR}/ImageAlphaPresentation.cpp
    ${PROJECT_SOURCE_DIR}/ImageBrowserMainImage.cpp
    ${PROJECT_SOURCE_DIR}/ImageAsyncBinding.cpp
)

target_include_directories(FICture2Tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/FD2D
    ${PROJECT_SOURCE_DIR}/ImageCore
    ${PROJECT_SOURCE_DIR}/external/spdlog/include
)

target_compile_definitions(FICture2Tests PRIVATE
    FD2D_STATIC
    SPDLOG_WCHAR_FILENAMES
    UNICODE
    _UNICODE
)

target_link_libraries(FICture2Tests PRIVATE
    FD2D
    ImageCore
    Floar
    DirectXTex
    d2d1
    dwrite
    windowscodecs
    d3d11
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_options(FICture2Tests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME FICture2.IpcOpenQueue COMMAND FICture2Tests IpcOpenQueue_)
add_test(NAME FICture2.ImageAsyncBinding COMMAND FICture2Tests ImageAsyncBinding_)
add_test(NAME FICture2.PathState COMMAND FICture2Tests PathState_)
add_test(NAME FICture2.GpuPresentation COMMAND FICture2Tests GpuPresentation_)
add_test(NAME FICture2.MainImage COMMAND FICture2Tests MainImage_)
# AppLog::Init runs once per process, so each Init scenario gets its own entry.
add_test(NAME FICture2.AppLogText COMMAND FICture2Tests AppLogText_)
add_test(NAME FICture2.AppLogLazy COMMAND FICture2Tests AppLogLazy_)
add_test(NAME FICture2.AppLogInitUnicode COMMAND FICture2Tests AppLogInitUnicode_)
add_test(NAME FICture2.AppLogInitFailure COMMAND FICture2Tests AppLogInitFailure_)
