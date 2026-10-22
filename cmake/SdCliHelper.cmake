# `qtmesh_sdcli` — stable-diffusion.cpp's own `sd-cli`, shipped next to the editor
# and driven out-of-process by SdCliHelper (src/SdCliHelper.{h,cpp}) for
# FLUX.3 Image Klein.
#
# WHY NOT just bump the sd.cpp the editor links (cmake block above): the linked
# sd.cpp shares llama.cpp's ggml (SD_USE_UPSTREAM_GGML), and a sd.cpp new enough
# for FLUX.3 aborts on that shared ggml in ggml_backend_sched_split_graph — a
# stride-0 int32 input the scheduler rejects — on EVERY model, FLUX.2 included.
# The same sd.cpp built with its OWN ggml fork generates FLUX.3 correctly (the
# unmodified `sd-cli`, verified end-to-end on Metal). So the new sd.cpp lives in
# an isolated sub-build, exactly like trellis.cpp (cmake/TrellisCpp.cmake): its
# ggml target names cannot collide with llama.cpp's, and the editor only
# consumes the one executable. The in-process sd.cpp stays on its older pin for
# texture generation and FLUX.2.
#
# Pin: b13a482 (2026-10-18) — FLUX.3 Image Klein + Klein Base support and docs.
# Re-test FLUX.3 generation when it moves.
#
# Self-contained binary: static ggml (BUILD_SHARED_LIBS=OFF), Metal shaders
# embedded (GGML_METAL_EMBED_LIBRARY), so on macOS it links only system
# frameworks and needs no sidecar files.
#
# Platforms: macOS (Metal) and Linux (CPU; Vulkan is a follow-up). Windows is
# not wired up yet — the option stays OFF there.
#
# Local/offline builds: -DQTMESH_SDCLI_SOURCE_DIR=<an sd.cpp checkout with its
# ggml submodule> skips the download.

if(TARGET qtmesh_sdcli OR NOT ENABLE_SD_CLI_HELPER)
    return()
endif()

include(ExternalProject)

set(QTMESH_SDCLI_SOURCE_DIR "" CACHE PATH
    "Use this sd.cpp checkout for the FLUX.3 helper instead of downloading one")

set(QTMESH_SDCLI_BINARY
    "${CMAKE_BINARY_DIR}/_deps/qtmesh_sdcli-build/bin/sd-cli"
    CACHE FILEPATH "Path of the built sd-cli helper executable" FORCE)

set(_sdcli_cmake_args
    -DCMAKE_BUILD_TYPE=Release
    -DSD_BUILD_EXAMPLES=ON
    -DSD_BUILD_SERVER=OFF
    # Self-contained executable (see header comment).
    -DBUILD_SHARED_LIBS=OFF
    # Portable CPU code, not -march=native: this binary SHIPS.
    -DGGML_NATIVE=OFF
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
if(APPLE)
    list(APPEND _sdcli_cmake_args -DSD_METAL=ON)
endif()
# Same AVX-without-FMA baseline as trellis-cli: ggml's x86_64 default needs
# Haswell+, which SIGILLs on older CPUs.
if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    list(APPEND _sdcli_cmake_args
         -DGGML_SSE42=ON -DGGML_AVX=ON -DGGML_AVX2=OFF
         -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_BMI2=OFF)
endif()
if(CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "|" _sdcli_osx_archs "${CMAKE_OSX_ARCHITECTURES}")
    list(APPEND _sdcli_cmake_args "-DCMAKE_OSX_ARCHITECTURES=${_sdcli_osx_archs}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _sdcli_cmake_args
         -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()

if(QTMESH_SDCLI_SOURCE_DIR)
    set(_sdcli_source_args SOURCE_DIR "${QTMESH_SDCLI_SOURCE_DIR}" DOWNLOAD_COMMAND "")
    set(QTMESH_SDCLI_SRC "${QTMESH_SDCLI_SOURCE_DIR}" CACHE INTERNAL "")
else()
    set(_sdcli_source_args
        GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
        GIT_TAG        b13a48219d5cdf408edcb93da6cd69b612ed03d3
        # Not shallow: a pinned commit stops resolving in a shallow clone as
        # soon as upstream master moves past it.
        GIT_SHALLOW    OFF
        # ggml is the one submodule sd-cli needs; skipping the web frontend and
        # image-codec submodules keeps the clone small.
        GIT_SUBMODULES ggml
        SOURCE_DIR     "${CMAKE_BINARY_DIR}/_deps/qtmesh_sdcli-src")
    set(QTMESH_SDCLI_SRC "${CMAKE_BINARY_DIR}/_deps/qtmesh_sdcli-src" CACHE INTERNAL "")
endif()

ExternalProject_Add(qtmesh_sdcli
    ${_sdcli_source_args}
    PREFIX         "${CMAKE_BINARY_DIR}/_deps/qtmesh_sdcli"
    BINARY_DIR     "${CMAKE_BINARY_DIR}/_deps/qtmesh_sdcli-build"
    LIST_SEPARATOR |
    CMAKE_ARGS     ${_sdcli_cmake_args}
    BUILD_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --target sd-cli
                   --config Release -j4
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${QTMESH_SDCLI_BINARY}"
    UPDATE_DISCONNECTED ON)
