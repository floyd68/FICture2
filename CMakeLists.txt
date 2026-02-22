cmake_minimum_required(VERSION 3.21)

project(FICture2 LANGUAGES CXX RC)

set(FICTURE2_DISTRIBUTION_CHANNEL "nexus_github" CACHE STRING "Distribution channel: nexus_github, windows_store, winget")
set_property(
    CACHE FICTURE2_DISTRIBUTION_CHANNEL
    PROPERTY STRINGS "nexus_github" "windows_store" "winget"
)

set(_default_build_thumbnail_provider ON)
set(_default_enable_first_run_prompts ON)
set(_default_enable_registration_menu ON)
set(_default_enable_msix_packaging OFF)
set(_default_enable_inno_setup OFF)
if(FICTURE2_DISTRIBUTION_CHANNEL STREQUAL "windows_store")
    set(_default_build_thumbnail_provider OFF)
    set(_default_enable_first_run_prompts OFF)
    set(_default_enable_registration_menu OFF)
    set(_default_enable_msix_packaging ON)
elseif(FICTURE2_DISTRIBUTION_CHANNEL STREQUAL "winget")
    set(_default_enable_first_run_prompts OFF)
    set(_default_enable_registration_menu OFF)
    set(_default_enable_inno_setup ON)
endif()

if(NOT DEFINED FICTURE2_BUILD_THUMBNAIL_PROVIDER)
    set(FICTURE2_BUILD_THUMBNAIL_PROVIDER ${_default_build_thumbnail_provider} CACHE BOOL "Build ThumbnailProvider DLL")
endif()
if(NOT DEFINED FICTURE2_ENABLE_FIRST_RUN_PROMPTS)
    set(FICTURE2_ENABLE_FIRST_RUN_PROMPTS ${_default_enable_first_run_prompts} CACHE BOOL "Enable first-run association prompts")
endif()
if(NOT DEFINED FICTURE2_ENABLE_REGISTRATION_MENU)
    set(FICTURE2_ENABLE_REGISTRATION_MENU ${_default_enable_registration_menu} CACHE BOOL "Enable registration menu actions")
endif()
if(NOT DEFINED FICTURE2_ENABLE_MSIX_PACKAGING)
    set(FICTURE2_ENABLE_MSIX_PACKAGING ${_default_enable_msix_packaging} CACHE BOOL "Enable MSIX packaging helpers")
endif()
if(NOT DEFINED FICTURE2_ENABLE_INNO_SETUP)
    set(FICTURE2_ENABLE_INNO_SETUP ${_default_enable_inno_setup} CACHE BOOL "Enable Inno Setup script generation")
endif()

option(FICTURE2_USE_DIRECTXTEX_OPENMP "Enable OpenMP for DirectXTex" OFF)
option(FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES "Enable ZIP/7z/RAR via libarchive" ON)
option(FICTURE2_ENABLE_PROFILING "Enable profiler-friendly compiler/linker flags" OFF)

set(BUILD_TESTING OFF CACHE BOOL "Disable tests by default" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use static runtime library (/MT) to eliminate VC Redistributable dependency
# This reduces network activity from Windows validating VC Runtime DLLs
# and removes the need to install VC Redistributable separately
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(FICTURE2_VERSION_MAJOR 1)
set(FICTURE2_VERSION_MINOR 4)
set(FICTURE2_VERSION_COMMITCOUNT 0)
set(FICTURE2_VERSION_BUILD 0)

execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE FICTURE2_GIT_COUNT_RESULT
    OUTPUT_VARIABLE FICTURE2_GIT_COUNT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(FICTURE2_GIT_COUNT_RESULT EQUAL 0 AND FICTURE2_GIT_COUNT_OUTPUT)
    set(FICTURE2_VERSION_COMMITCOUNT ${FICTURE2_GIT_COUNT_OUTPUT})
endif()

if(DEFINED ENV{BUILD_NUMBER})
    set(FICTURE2_VERSION_BUILD $ENV{BUILD_NUMBER})
elseif(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(FICTURE2_VERSION_BUILD $ENV{GITHUB_RUN_NUMBER})
elseif(DEFINED ENV{CI_PIPELINE_IID})
    set(FICTURE2_VERSION_BUILD $ENV{CI_PIPELINE_IID})
endif()

set(FICTURE2_VERSION_STRING
    "${FICTURE2_VERSION_MAJOR}.${FICTURE2_VERSION_MINOR}.${FICTURE2_VERSION_COMMITCOUNT}.${FICTURE2_VERSION_BUILD}"
    CACHE STRING "FICture2 version string" FORCE
)

set(FICTURE2_BUILD_FLAVOR "standalone")
if(FICTURE2_DISTRIBUTION_CHANNEL STREQUAL "windows_store")
    set(FICTURE2_BUILD_FLAVOR "store")
elseif(FICTURE2_DISTRIBUTION_CHANNEL STREQUAL "winget")
    set(FICTURE2_BUILD_FLAVOR "winget")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.h
    @ONLY
)

if(FICTURE2_ENABLE_MSIX_PACKAGING)
    set(FICTURE2_APPX_IDENTITY_NAME "FICture2" CACHE STRING "MSIX identity name")
    set(FICTURE2_APPX_PUBLISHER "CN=FICture2" CACHE STRING "MSIX publisher")
    set(FICTURE2_APPX_DISPLAY_NAME "FICture2" CACHE STRING "MSIX display name")
    set(FICTURE2_APPX_EXECUTABLE "FICture2.exe" CACHE STRING "MSIX executable")
    set(FICTURE2_APPX_LOGO_PATH "assets\\StoreLogo.png" CACHE STRING "MSIX store logo path")
    set(FICTURE2_APPX_FILETYPE_GROUP "FICture2.Image" CACHE STRING "MSIX file type group name")
    set(FICTURE2_APPX_SUPPORTED_FILE_TYPES
        ".png;.jpg;.jpeg;.bmp;.tif;.tiff;.gif;.dds;.tga;.ico"
        CACHE STRING "MSIX supported file types (semicolon separated)"
    )

    set(_appx_filetype_entries "")
    foreach(ext IN LISTS FICTURE2_APPX_SUPPORTED_FILE_TYPES)
        string(APPEND _appx_filetype_entries "          <uap:FileType>${ext}</uap:FileType>\n")
    endforeach()
    set(FICTURE2_APPX_FILETYPE_ENTRIES "${_appx_filetype_entries}")

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/AppxManifest.xml.in
        ${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml
        @ONLY
    )
    set(FICTURE2_APPX_MANIFEST_PATH "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml" CACHE PATH "Generated Appx manifest")
endif()

if(FICTURE2_ENABLE_INNO_SETUP)
    set(FICTURE2_INNO_APP_NAME "FICture2" CACHE STRING "Inno Setup app name")
    set(FICTURE2_INNO_PUBLISHER "floyd68" CACHE STRING "Inno Setup publisher")
    set(FICTURE2_INNO_EXE_NAME "FICture2.exe" CACHE STRING "Inno Setup executable name")
    set(
        FICTURE2_INNO_OUTPUT_NAME
        "FICture2-Setup-${FICTURE2_VERSION_MAJOR}.${FICTURE2_VERSION_MINOR}"
        CACHE STRING "Inno Setup output filename"
    )
    set(FICTURE2_INNO_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/F2.ico" CACHE STRING "Inno Setup icon path")
    set(FICTURE2_INNO_BUILD_CONFIG "MinSizeRel" CACHE STRING "Inno Setup build config")
    set(
        FICTURE2_INNO_EXE_PATH
        "${CMAKE_BINARY_DIR}/bin/${FICTURE2_INNO_BUILD_CONFIG}/${FICTURE2_INNO_EXE_NAME}"
    )
    set(
        FICTURE2_INNO_THUMBNAIL_PROVIDER_PATH
        "${CMAKE_BINARY_DIR}/bin/${FICTURE2_INNO_BUILD_CONFIG}/ThumbnailProvider.dll"
    )
    file(TO_NATIVE_PATH "${FICTURE2_INNO_ICON_PATH}" FICTURE2_INNO_ICON_PATH)
    file(TO_NATIVE_PATH "${FICTURE2_INNO_EXE_PATH}" FICTURE2_INNO_EXE_PATH)
    file(TO_NATIVE_PATH "${FICTURE2_INNO_THUMBNAIL_PROVIDER_PATH}" FICTURE2_INNO_THUMBNAIL_PROVIDER_PATH)
    if(FICTURE2_BUILD_THUMBNAIL_PROVIDER)
        set(FICTURE2_INNO_INSTALL_THUMBNAIL_PROVIDER "True")
    else()
        set(FICTURE2_INNO_INSTALL_THUMBNAIL_PROVIDER "False")
    endif()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/FICture2.iss.in
        ${CMAKE_CURRENT_BINARY_DIR}/FICture2.iss
        @ONLY
    )
    set(FICTURE2_INNO_SCRIPT_PATH "${CMAKE_CURRENT_BINARY_DIR}/FICture2.iss" CACHE PATH "Generated Inno Setup script")
endif()

if(FICTURE2_DISTRIBUTION_CHANNEL STREQUAL "winget")
    set(FICTURE2_WINGET_PACKAGE_ID "floyd68.FICture2" CACHE STRING "winget package identifier")
    set(FICTURE2_WINGET_INSTALLER_URL "" CACHE STRING "winget installer URL")
    set(FICTURE2_WINGET_INSTALLER_SHA256 "" CACHE STRING "winget installer SHA256")
    
    # Get current date for release date
    string(TIMESTAMP FICTURE2_RELEASE_DATE "%Y-%m-%d" UTC)
    
    # Generate all three manifest files required by winget
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/winget/FICture2.yaml.in
        ${CMAKE_CURRENT_BINARY_DIR}/winget_manifests/${FICTURE2_WINGET_PACKAGE_ID}.yaml
        @ONLY
    )
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/winget/FICture2.installer.yaml.in
        ${CMAKE_CURRENT_BINARY_DIR}/winget_manifests/${FICTURE2_WINGET_PACKAGE_ID}.installer.yaml
        @ONLY
    )
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/winget/FICture2.locale.en-US.yaml.in
        ${CMAKE_CURRENT_BINARY_DIR}/winget_manifests/${FICTURE2_WINGET_PACKAGE_ID}.locale.en-US.yaml
        @ONLY
    )
    set(FICTURE2_WINGET_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/winget_manifests" CACHE PATH "Generated winget manifests directory")
endif()

function(set_cache_bool name value description)
    set(${name} ${value} CACHE BOOL "${description}" FORCE)
endfunction()

function(set_cache_string name value description)
    set(${name} ${value} CACHE STRING "${description}" FORCE)
endfunction()

function(set_cache_path name value description)
    set(${name} ${value} CACHE PATH "${description}" FORCE)
endfunction()

function(set_cache_bools_from_pairs pairs_list description_prefix)
    foreach(entry IN LISTS ${pairs_list})
        string(REPLACE "=" ";" parts "${entry}")
        list(LENGTH parts parts_len)
        if(parts_len LESS 2)
            message(FATAL_ERROR "Expected NAME=VALUE pair, got: ${entry}")
        endif()
        list(GET parts 0 name)
        list(GET parts 1 value)
        set_cache_bool(${name} ${value} "${description_prefix} ${name}")
    endforeach()
endfunction()

function(set_cache_bools_off names_list description_prefix)
    foreach(name IN LISTS ${names_list})
        set_cache_bool(${name} OFF "${description_prefix} ${name}")
    endforeach()
endfunction()

# DirectXTex
set_cache_bool(BUILD_TOOLS OFF "Disable DirectXTex tools")
set_cache_bool(BUILD_SAMPLE OFF "Disable DirectXTex sample")
set_cache_bool(BUILD_SHARED_LIBS OFF "Static DirectXTex")
if(FICTURE2_USE_DIRECTXTEX_OPENMP)
    set_cache_bool(BC_USE_OPENMP ON "DirectXTex OpenMP support")
endif()
add_subdirectory(external/DirectXTex)

# zlib (BA2 support)
set_cache_bool(ZLIB_BUILD_SHARED OFF "Disable zlib shared")
set_cache_bool(ZLIB_BUILD_STATIC ON "Enable zlib static")
set_cache_bool(ZLIB_BUILD_TESTING OFF "Disable zlib tests")
set_cache_bool(ZLIB_INSTALL OFF "Disable zlib install")
add_subdirectory(external/zlib)
set_cache_string(ZLIB_LIBRARY zlibstatic "Use local zlib")
set_cache_string(
    ZLIB_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib;${CMAKE_BINARY_DIR}/external/zlib"
    "Use local zlib includes"
)

# libarchive + xz (ZIP/7z/RAR)
if(FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES)
    set(_xz_bool_options
        XZ_NLS
        XZ_DOC
        XZ_DOXYGEN
        XZ_TOOL_XZ
        XZ_TOOL_XZDEC
        XZ_TOOL_LZMADEC
        XZ_TOOL_LZMAINFO
        XZ_TOOL_SCRIPTS
        XZ_TOOL_SYMLINKS
        XZ_TOOL_SYMLINKS_LZMA
    )
    set_cache_bools_off(_xz_bool_options "Disable xz")
    set_cache_bool(BUILD_SHARED_LIBS OFF "Static xz")
    add_subdirectory(external/xz)
    set(_liblzma_config_dir "${CMAKE_BINARY_DIR}/_liblzma_config")
    file(MAKE_DIRECTORY "${_liblzma_config_dir}")
    file(WRITE "${_liblzma_config_dir}/LibLZMAConfig.cmake"
        "set(LIBLZMA_FOUND TRUE)\n"
        "set(LIBLZMA_INCLUDE_DIRS \"${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api\")\n"
        "set(LIBLZMA_LIBRARIES liblzma)\n")
    file(WRITE "${_liblzma_config_dir}/liblzma-config.cmake"
        "include(\"${_liblzma_config_dir}/LibLZMAConfig.cmake\")\n")

    set_cache_string(POSIX_REGEX_LIB LIBC "Use libc regex")
    set(_libarchive_cache_bools
        ENABLE_WERROR=OFF
        ENABLE_MBEDTLS=OFF
        ENABLE_NETTLE=OFF
        ENABLE_OPENSSL=OFF
        ENABLE_LIBB2=OFF
        ENABLE_LZ4=OFF
        ENABLE_LZO=OFF
        ENABLE_LZMA=ON
        ENABLE_ZSTD=OFF
        ENABLE_ZLIB=ON
        ENABLE_BZip2=OFF
        ENABLE_LIBXML2=OFF
        ENABLE_EXPAT=OFF
        ENABLE_WIN32_XMLLITE=OFF
        ENABLE_PCREPOSIX=OFF
        ENABLE_PCRE2POSIX=OFF
        ENABLE_CNG=OFF
        ENABLE_TAR=OFF
        ENABLE_CPIO=OFF
        ENABLE_CAT=OFF
        ENABLE_UNZIP=OFF
        ENABLE_XATTR=OFF
        ENABLE_ACL=OFF
        ENABLE_ICONV=OFF
        ENABLE_TEST=OFF
        ENABLE_INSTALL=OFF
        BUILD_SHARED_LIBS=OFF
    )
    set_cache_bools_from_pairs(_libarchive_cache_bools "libarchive")

    set_cache_path(LibLZMA_DIR "${CMAKE_BINARY_DIR}/_liblzma_config" "Use local liblzma config")
    set_cache_bool(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON "Prefer config packages")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/external/xz")
    set_cache_string(
        LIBLZMA_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api"
        "Use local liblzma include"
    )
    set_cache_path(LIBLZMA_LIBRARY_RELEASE "${CMAKE_BINARY_DIR}/external/xz/Release/lzma.lib" "Use local liblzma release")
    set_cache_path(LIBLZMA_LIBRARY_DEBUG "${CMAKE_BINARY_DIR}/external/xz/Debug/lzma.lib" "Use local liblzma debug")

    add_subdirectory(external/libarchive)
endif()

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
    ImageBrowserAsyncThumbLoader.cpp
    ImageBrowserContextMenu.cpp
    ImageBrowserDeferredActions.cpp
    ImageBrowserDragController.cpp
    ImageBrowserAssets.cpp
    ImageBrowserInfoPresenter.cpp
    ImageBrowserKeyboardController.cpp
    ImageBrowserMainPane.cpp
    ImageBrowserSessionPersistence.cpp
    ImageBrowserSplitCoordinator.cpp
    ImageBrowserThumbnailPane.cpp
    ImageBrowserThumbLayoutCoordinator.cpp
    ImageBrowserThumbStripController.cpp
    ThumbImageTile.cpp
    ThumbNavTile.cpp
    VirtualFileSystem.cpp
    VirtualPath.cpp
    app.manifest
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
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(FICture2 PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
    _WINDOWS
    $<$<BOOL:${FICTURE2_ENABLE_FIRST_RUN_PROMPTS}>:FICTURE2_ENABLE_FIRST_RUN_PROMPTS=1>
    $<$<NOT:$<BOOL:${FICTURE2_ENABLE_FIRST_RUN_PROMPTS}>>:FICTURE2_ENABLE_FIRST_RUN_PROMPTS=0>
    $<$<BOOL:${FICTURE2_ENABLE_REGISTRATION_MENU}>:FICTURE2_ENABLE_REGISTRATION_MENU=1>
    $<$<NOT:$<BOOL:${FICTURE2_ENABLE_REGISTRATION_MENU}>>:FICTURE2_ENABLE_REGISTRATION_MENU=0>
    $<$<STREQUAL:${FICTURE2_BUILD_FLAVOR},standalone>:FICTURE2_BUILD_FLAVOR_STANDALONE=1>
    $<$<NOT:$<STREQUAL:${FICTURE2_BUILD_FLAVOR},standalone>>:FICTURE2_BUILD_FLAVOR_STANDALONE=0>
    $<$<STREQUAL:${FICTURE2_BUILD_FLAVOR},winget>:FICTURE2_BUILD_FLAVOR_WINGET=1>
    $<$<NOT:$<STREQUAL:${FICTURE2_BUILD_FLAVOR},winget>>:FICTURE2_BUILD_FLAVOR_WINGET=0>
    $<$<STREQUAL:${FICTURE2_BUILD_FLAVOR},store>:FICTURE2_BUILD_FLAVOR_STORE=1>
    $<$<NOT:$<STREQUAL:${FICTURE2_BUILD_FLAVOR},store>>:FICTURE2_BUILD_FLAVOR_STORE=0>
    $<$<BOOL:${FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES}>:FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES=1>
    $<$<NOT:$<BOOL:${FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES}>>:FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES=0>
)

target_link_libraries(FICture2 PRIVATE
    FD2D
    ImageCore
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

if(FICTURE2_ENABLE_LIBARCHIVE_COMMON_ARCHIVES)
    target_include_directories(FICture2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libarchive/libarchive
    )
    target_compile_definitions(FICture2 PRIVATE LIBARCHIVE_STATIC)
    target_link_libraries(FICture2 PRIVATE archive_static)
else()
    target_include_directories(FICture2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/zlib
        ${CMAKE_BINARY_DIR}/external/zlib
    )
    target_link_libraries(FICture2 PRIVATE zlibstatic)
endif()

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /arch:SSE2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /arch:SSE2")
    set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} /arch:SSE2 /Os /GL /Oy")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /arch:SSE2 /Os /GL /Oy")
    set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL} /LTCG")

    if(FICTURE2_ENABLE_PROFILING)
        # Profiling-friendly setup:
        # - keep frame pointers for reliable sampled callstacks
        # - emit full debug symbols (PDB)
        # - disable LTCG/whole-program optimization in MinSizeRel for clearer attribution
        string(REPLACE "/GL" "" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")
        string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
        string(REPLACE "/LTCG" "" CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL}")
        string(REPLACE "/Oy" "" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")
        string(REPLACE "/Oy" "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
        string(REPLACE "/Oy" "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
        string(REPLACE "/Oy" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

        add_compile_options(
            $<$<CONFIG:MinSizeRel>:/Zi /Oy->
            $<$<CONFIG:RelWithDebInfo>:/Zi /Oy->
        )
        add_link_options(
            $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        )
    endif()

    target_compile_options(FICture2 PRIVATE /permissive-)
endif()
