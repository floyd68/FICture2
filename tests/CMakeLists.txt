# FICture2 first-party unit tests (enabled with -DFICTURE2_BUILD_TESTS=ON).
# Non-window tests only; vendored dependency tests stay disabled.

add_executable(FICture2Tests
    TestHarness.h
    TestMain.cpp
    ImageAsyncBindingTests.cpp
    IpcOpenQueueTests.cpp
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
    UNICODE
    _UNICODE
)

target_link_libraries(FICture2Tests PRIVATE
    FD2D
    ImageCore
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
