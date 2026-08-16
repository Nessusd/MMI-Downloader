cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MMID_PROJECT_VERSION)
string(STRIP "${MMID_PROJECT_VERSION}" MMID_PROJECT_VERSION)
if(NOT MMID_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 1.2.3")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(MMIDownloader VERSION "${MMID_PROJECT_VERSION}" LANGUAGES C CXX RC)

if(DEFINED ENV{MMID_BUILD_NUMBER} AND NOT "$ENV{MMID_BUILD_NUMBER}" STREQUAL "")
    set(MMID_BUILD_NUMBER "$ENV{MMID_BUILD_NUMBER}")
else()
    string(TIMESTAMP MMID_BUILD_NUMBER "%Y%m%d%H%M")
endif()
configure_file(
    src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)
configure_file(
    src/MMIDownloaderVersion.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/MMIDownloaderVersion.rc
    @ONLY
)
configure_file(
    src/MMIDownloader.manifest.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/MMIDownloader.manifest
    @ONLY
)
# The upstream zlib repository intentionally does not track zconf.h.  Copy the
# release template into the build tree so a clean checkout does not depend on
# a locally generated file left in third_party/zlib.
configure_file(
    third_party/zlib/zconf.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/zconf.h
    COPYONLY
)

add_library(zlib_static STATIC
    third_party/zlib/adler32.c
    third_party/zlib/crc32.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/zutil.c
)
target_include_directories(zlib_static PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    third_party/zlib
)
target_compile_definitions(zlib_static PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(bzip2_static STATIC
    third_party/bzlib/blocksort.c
    third_party/bzlib/bzlib.c
    third_party/bzlib/compress.c
    third_party/bzlib/crctable.c
    third_party/bzlib/decompress.c
    third_party/bzlib/huffman.c
    third_party/bzlib/randtable.c
    src/bzip2_error.c
)
target_include_directories(bzip2_static PUBLIC third_party/bzlib)
target_compile_definitions(bzip2_static PRIVATE _CRT_SECURE_NO_WARNINGS BZ_EXPORT BZ_NO_STDIO)

add_library(mmid_core STATIC
    src/core.cpp
)
target_include_directories(mmid_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(mmid_core PRIVATE
    bzip2_static
    zlib_static
    winhttp
    wininet
    crypt32
    bcrypt
    advapi32
    shell32
    ole32
    shlwapi
    xmllite
)

add_executable(MMIDownloader WIN32
    src/gui.cpp
    src/MMIDownloader.rc
    ${CMAKE_CURRENT_BINARY_DIR}/generated/MMIDownloaderVersion.rc
)

target_include_directories(MMIDownloader PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(mmid_core PUBLIC cxx_std_20)
target_compile_features(MMIDownloader PRIVATE cxx_std_20)

foreach(target IN ITEMS mmid_core MMIDownloader)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WINVER=0x0601
        _WIN32_WINNT=0x0601
        NTDDI_VERSION=0x06010000
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endforeach()

if(MSVC)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)
    include(CheckLinkerFlag)

    set_property(TARGET zlib_static PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET bzip2_static PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET mmid_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET MMIDownloader PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_link_options(MMIDownloader PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/generated/MMIDownloader.manifest")

    # Compiler-option probes do not need a linker. Avoid treating transient
    # linker/output-file failures as evidence that a compiler flag is absent.
    set(_MMID_SAVED_TRY_COMPILE_TARGET_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_c_compiler_flag("/guard:cf" MMID_C_HAS_GUARD_CF_COMPILE)
    check_cxx_compiler_flag("/guard:cf" MMID_CXX_HAS_GUARD_CF_COMPILE)
    check_cxx_compiler_flag("/sdl" MMID_CXX_HAS_SDL)
    if(_MMID_SAVED_TRY_COMPILE_TARGET_TYPE)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_MMID_SAVED_TRY_COMPILE_TARGET_TYPE}")
    else()
        unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
    endif()
    unset(_MMID_SAVED_TRY_COMPILE_TARGET_TYPE)
    check_linker_flag(CXX "/guard:cf" MMID_LINKER_HAS_GUARD_CF)

    # CFG-enabled images remain loadable on CFG-unaware Windows versions,
    # including the Windows 7 target. Enable it only when the complete active
    # toolchain accepts both compiler and linker switches.
    if(MMID_C_HAS_GUARD_CF_COMPILE AND MMID_CXX_HAS_GUARD_CF_COMPILE AND MMID_LINKER_HAS_GUARD_CF)
        target_compile_options(zlib_static PRIVATE /guard:cf)
        target_compile_options(bzip2_static PRIVATE /guard:cf)
        target_compile_options(mmid_core PRIVATE /guard:cf)
        target_compile_options(MMIDownloader PRIVATE /guard:cf)
        target_link_options(MMIDownloader PRIVATE /guard:cf)
        message(STATUS "Control Flow Guard enabled")
    else()
        message(WARNING "Control Flow Guard is unavailable in this MSVC toolchain")
    endif()

    target_compile_options(mmid_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(MMIDownloader PRIVATE /W4 /permissive- /utf-8)
    if(MMID_CXX_HAS_SDL)
        target_compile_options(mmid_core PRIVATE /sdl)
        target_compile_options(MMIDownloader PRIVATE /sdl)
        message(STATUS "MSVC SDL checks enabled")
    else()
        message(WARNING "MSVC SDL checks are unavailable in this toolchain")
    endif()
endif()

target_link_libraries(MMIDownloader PRIVATE
    mmid_core
    shell32
    ole32
    comctl32
    gdi32
)
