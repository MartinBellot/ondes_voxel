cmake_minimum_required(VERSION 3.25)

# vcpkg toolchain, if bootstrap.sh has cloned it and no toolchain was given.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(OndesVoxel
    VERSION 0.0.1
    DESCRIPTION "A C++ reimplementation of Minecraft Java Edition 1.20.1"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Options ─────────────────────────────────────────────────────────────────
option(OV_BUILD_CLIENT "Build the renderer and the playable client" ON)
option(OV_BUILD_TESTS  "Build unit tests and benchmarks"            ON)
option(OV_WERROR       "Treat warnings as errors"                   ON)

# LTO is deliberately OFF by default. A full-LTO link of this project exceeds
# 6 GB and makes an 8 GB machine swap; it belongs in a release CI job only.
# See risk R5 in docs/ARCHITECTURE.md — build velocity is what kills projects
# this size, and it is the risk everyone treats as cosmetic.
option(OV_LTO "Enable link-time optimization (release CI only)" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(OV_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ov_ipo OUTPUT _ov_ipo_msg)
    if(_ov_ipo)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO requested but unsupported: ${_ov_ipo_msg}")
    endif()
endif()

# ccache, if present. Non-negotiable for day-to-day velocity (risk R5).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

include(OvCompilerCheck)
ov_check_cxx23_support()

include(OvWarnings)
include(OvSanitizers)
include(OvModule)
include(OvHeaderSelfTest)
include(OvShaders)

# ── Third-party ─────────────────────────────────────────────────────────────
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(libdeflate CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)

if(OV_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

# ── Modules ─────────────────────────────────────────────────────────────────
add_subdirectory(src/ov_base)
add_subdirectory(src/ov_math)
add_subdirectory(src/ov_io)
add_subdirectory(src/ov_nbt)
add_subdirectory(src/ov_registry)
add_subdirectory(src/ov_world)
add_subdirectory(src/ov_protocol)
add_subdirectory(src/ov_entity)
add_subdirectory(src/ov_gameplay)
add_subdirectory(src/ov_worldgen)
add_subdirectory(src/ov_netclient)
add_subdirectory(src/ov_server)

# ── Rendering frontier ──────────────────────────────────────────────────────
# Everything below this line is client-only. The linux-server-only preset turns
# OV_BUILD_CLIENT off, so the headless job never compiles a line of it — which
# is what makes layering lock #3 a real guarantee rather than a policy check.
if(OV_BUILD_CLIENT)
    find_package(volk CONFIG REQUIRED)
    find_package(VulkanHeaders CONFIG REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(glfw3 CONFIG REQUIRED)

    # ── sound ── miniaudio and stb_vorbis ship as a header and a .c with no
    # CMake package, so they are found as files and exposed as one interface
    # target that ov_audio alone takes as a PRIVATE_DEP. Declared here and not
    # under src/, where scripts/check_layers.py forbids target_link_libraries.
    find_path(OV_MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
    find_path(OV_STB_VORBIS_INCLUDE_DIR stb_vorbis.c REQUIRED)
    add_library(thirdparty_audio INTERFACE)
    target_include_directories(thirdparty_audio SYSTEM INTERFACE
        ${OV_MINIAUDIO_INCLUDE_DIR} ${OV_STB_VORBIS_INCLUDE_DIR})
    if(APPLE)
        target_link_libraries(thirdparty_audio INTERFACE
            "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    elseif(UNIX)
        target_link_libraries(thirdparty_audio INTERFACE ${CMAKE_DL_LIBS} m)
    endif()
    # ── end sound ──

    add_subdirectory(src/ov_rhi)
    add_subdirectory(src/ov_render)
    add_subdirectory(src/ov_audio)  # ── sound ──
    add_subdirectory(src/ov_client)
endif()

# ── Checks ──────────────────────────────────────────────────────────────────
# Compiles every public header on its own. Catches the header that only works
# because something else included <string_view> first — which compiles on
# libc++ and fails on libstdc++.
set(OV_SELFTEST_MODULES ov_base ov_math ov_io ov_nbt ov_registry ov_world ov_protocol ov_entity)
if(OV_BUILD_CLIENT)
    list(APPEND OV_SELFTEST_MODULES ov_rhi ov_render ov_audio ov_client)
endif()
ov_add_header_selftest(MODULES ${OV_SELFTEST_MODULES})

# ── Apps ────────────────────────────────────────────────────────────────────
add_subdirectory(apps/ov_dedicated)

if(OV_BUILD_CLIENT)
    add_subdirectory(apps/ov_voxel)
endif()

# ── Tools ───────────────────────────────────────────────────────────────────
add_subdirectory(tools/ov_inspect)
add_subdirectory(tools/ov_parity)
add_subdirectory(tools/ov_features)
add_subdirectory(tools/ov_genparity)
add_subdirectory(tools/ov_gendet)
add_subdirectory(tools/ov_carveparity)
add_subdirectory(tools/ov_surfparity)
add_subdirectory(tools/ov_netherparity)
add_subdirectory(tools/ov_endparity)  # ── end ──
add_subdirectory(tools/ov_structparity)
add_subdirectory(tools/ov_structblocks)  # ── structures-gabarits ──
add_subdirectory(tools/ov_jigsawparity)  # ── jigsaw ──
add_subdirectory(tools/ov_caveedge)
add_subdirectory(tools/ov_lab)
add_subdirectory(tools/ov_mobloot)
add_subdirectory(tools/ov_playerdata)
add_subdirectory(tools/ov_assetimport)

if(OV_BUILD_CLIENT)
    add_subdirectory(tools/ov_modelbake)
endif()

# ── Summary ─────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "Ondes VOXEL ${PROJECT_VERSION} — target: Minecraft Java Edition 1.20.1 (protocol 763)")
message(STATUS "  client ....... ${OV_BUILD_CLIENT}")
message(STATUS "  tests ........ ${OV_BUILD_TESTS}")
message(STATUS "  werror ....... ${OV_WERROR}")
message(STATUS "  lto .......... ${OV_LTO}")
message(STATUS "  sanitizer .... ${OV_SANITIZER}")
message(STATUS "")
