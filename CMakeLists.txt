cmake_minimum_required(VERSION 3.21)

project(FICture2 LANGUAGES CXX RC)

option(FICTURE2_BUILD_THUMBNAIL_PROVIDER "Build ThumbnailProvider DLL" ON)
option(FICTURE2_USE_LOCAL_ARCHIVE_DEPS "Use local zlib/xz submodules" ON)
option(FICTURE2_USE_DIRECTXTEX_OPENMP "Enable OpenMP for DirectXTex" OFF)
set(BUILD_TESTING OFF CACHE BOOL "Disable tests by default" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_TOOLS OFF CACHE BOOL "Disable DirectXTex tools" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "Disable DirectXTex sample" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static DirectXTex" FORCE)
if(FICTURE2_USE_DIRECTXTEX_OPENMP)
    set(BC_USE_OPENMP ON CACHE BOOL "DirectXTex OpenMP support" FORCE)
endif()
add_subdirectory(external/DirectXTex)

if(FICTURE2_USE_LOCAL_ARCHIVE_DEPS)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Disable zlib shared" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "Enable zlib static" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Disable zlib tests" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "Disable zlib install" FORCE)
    add_subdirectory(external/zlib)

    set(XZ_NLS OFF CACHE BOOL "Disable xz NLS" FORCE)
    set(XZ_DOC OFF CACHE BOOL "Disable xz docs" FORCE)
    set(XZ_DOXYGEN OFF CACHE BOOL "Disable xz doxygen" FORCE)
    set(XZ_TOOL_XZ OFF CACHE BOOL "Disable xz tool" FORCE)
    set(XZ_TOOL_XZDEC OFF CACHE BOOL "Disable xzdec tool" FORCE)
    set(XZ_TOOL_LZMADEC OFF CACHE BOOL "Disable lzmadec tool" FORCE)
    set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "Disable lzmainfo tool" FORCE)
    set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "Disable xz scripts" FORCE)
    set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "Disable xz symlinks" FORCE)
    set(XZ_TOOL_SYMLINKS_LZMA OFF CACHE BOOL "Disable xz lzma symlinks" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static xz" FORCE)
    add_subdirectory(external/xz)
    set(_liblzma_config_dir "${CMAKE_BINARY_DIR}/_liblzma_config")
    file(MAKE_DIRECTORY "${_liblzma_config_dir}")
    file(WRITE "${_liblzma_config_dir}/LibLZMAConfig.cmake"
        "set(LIBLZMA_FOUND TRUE)\n"
        "set(LIBLZMA_INCLUDE_DIRS \"${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api\")\n"
        "set(LIBLZMA_LIBRARIES liblzma)\n")
    file(WRITE "${_liblzma_config_dir}/liblzma-config.cmake"
        "include(\"${_liblzma_config_dir}/LibLZMAConfig.cmake\")\n")
endif()

set(POSIX_REGEX_LIB LIBC CACHE STRING "Use libc regex" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "Disable libarchive WERROR" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "Disable libarchive mbedtls" FORCE)
set(ENABLE_NETTLE OFF CACHE BOOL "Disable libarchive nettle" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "Disable libarchive openssl" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "Disable libarchive libb2" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "Disable libarchive lz4" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "Disable libarchive lzo" FORCE)
set(ENABLE_LZMA ON CACHE BOOL "Enable libarchive lzma" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "Disable libarchive zstd" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "Enable libarchive zlib" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "Disable libarchive bzip2" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "Disable libarchive libxml2" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "Disable libarchive expat" FORCE)
set(ENABLE_WIN32_XMLLITE OFF CACHE BOOL "Disable libarchive xmllite" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "Disable libarchive pcreposix" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "Disable libarchive pcre2posix" FORCE)
set(ENABLE_LIBGCC OFF CACHE BOOL "Disable libarchive libgcc" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "Disable libarchive CNG" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "Disable libarchive tar" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "Disable libarchive cpio" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "Disable libarchive cat" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "Disable libarchive unzip" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "Disable libarchive xattr" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "Disable libarchive acl" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "Disable libarchive iconv" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "Disable libarchive tests" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "Disable libarchive install" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static libarchive" FORCE)

if(FICTURE2_USE_LOCAL_ARCHIVE_DEPS)
    set(ZLIB_LIBRARY zlibstatic CACHE STRING "Use local zlib" FORCE)
    set(ZLIB_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib;${CMAKE_BINARY_DIR}/external/zlib"
        CACHE STRING "Use local zlib includes" FORCE)
    set(LibLZMA_DIR
        "${CMAKE_BINARY_DIR}/_liblzma_config"
        CACHE PATH "Use local liblzma config" FORCE)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "Prefer config packages" FORCE)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/external/xz")
    set(LIBLZMA_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api"
        CACHE STRING "Use local liblzma include" FORCE)
    set(LIBLZMA_LIBRARY_RELEASE
        "${CMAKE_BINARY_DIR}/external/xz/Release/lzma.lib"
        CACHE FILEPATH "Use local liblzma release" FORCE)
    set(LIBLZMA_LIBRARY_DEBUG
        "${CMAKE_BINARY_DIR}/external/xz/Debug/lzma.lib"
        CACHE FILEPATH "Use local liblzma debug" FORCE)
endif()

add_subdirectory(external/libarchive)
add_subdirectory(ImageCore)
add_subdirectory(FD2D)

if(FICTURE2_BUILD_THUMBNAIL_PROVIDER)
    add_subdirectory(ThumbnailProvider)
endif()

add_executable(FICture2 WIN32)

target_sources(FICture2 PRIVATE
    AppIpc.cpp
    AppSetup.cpp
    ArchiveReader.cpp
    FICture2.cpp
    Ficture2Backplate.cpp
    ImageBrowser.cpp
    ImageBrowserDragDrop.cpp
    ImageBrowserDragOverlay.cpp
    ImageBrowserInputController.cpp
    ImageBrowserMainPane.cpp
    ImageBrowserNavigation.cpp
    ImageBrowserThumbStripController.cpp
    ThumbImageTile.cpp
    ThumbNavTile.cpp
    VirtualFileSystem.cpp
    VirtualPath.cpp
    FICture2.rc
)

set(FICTURE2_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set_target_properties(FICture2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FICTURE2_RUNTIME_OUTPUT_DIR}"
)
if(TARGET ThumbnailProvider)
    set_target_properties(ThumbnailProvider PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${FICTURE2_RUNTIME_OUTPUT_DIR}"
    )
endif()

target_include_directories(FICture2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/FD2D
    ${CMAKE_CURRENT_SOURCE_DIR}/ImageCore
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libarchive/libarchive
)

target_compile_definitions(FICture2 PRIVATE
    FD2D_STATIC
    LIBARCHIVE_STATIC
    UNICODE
    _UNICODE
    _WINDOWS
)

target_link_libraries(FICture2 PRIVATE
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

target_link_libraries(FICture2 PRIVATE archive_static)

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /arch:SSE2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /arch:SSE2")
    set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} /arch:SSE2 /Os /GL /Oy")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /arch:SSE2 /Os /GL /Oy")
    set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL} /LTCG")
    target_compile_options(FICture2 PRIVATE /permissive-)
endif()
