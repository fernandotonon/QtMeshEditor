# Bundled trellis.cpp runtime (TRELLIS.2 image-to-3D, MIT code + weights).
#
# Builds pwilkin/trellis.cpp's `trellis-cli` as part of the QtMeshEditor build
# and ships it next to the editor binary, so TRELLIS.2 needs NO separate
# runtime install — only the GGUF weights, which AI Model Settings downloads
# into <AppData>/ai_models/trellis2/ (a directory Trellis2Predictor already
# resolves). Pinned to upstream main AFTER both of our PRs merged (#44 Metal
# support, #45 --dump-post with the cleaned remesh) — re-audit licenses if the
# pin moves (docs/trellis2-dependencies.md).
#
# Built as an EXTERNAL PROJECT (isolated sub-build), NOT FetchContent:
# trellis.cpp vendors its own patched ggml fork whose target names collide
# with the ggml that llama.cpp (ENABLE_LOCAL_LLM) already brings into this
# build. The sub-build has its own namespace and we only consume the one
# produced executable (+ any ggml shared libs if the static build falls back).
#
# BUILD_SHARED_LIBS=OFF is REQUIRED for shipping: ggml defaults to shared on
# Linux, and a dynamically-linked trellis-cli needs libggml{,-cpu,-base}.so.0
# next to it ($ORIGIN). 3.37.3/3.37.4 shipped only the CLI → exit 127
# ("error while loading shared libraries") on every .deb/snap install.
# Static ggml folds those into the CLI so the package is self-contained.
# CopyTrellisRuntime.cmake still scoops up any leftover shared libs as a
# safety net for backend-as-DLL layouts.
#
# Backends: ggml picks Metal automatically on Apple; everywhere else this
# builds the CPU backend (slow but functional baseline — GPU builds remain a
# power-user recompile with -DGGML_VULKAN/CUDA on the trellis.cpp side).
# TRELLIS_WEBP is forced OFF: QtMeshEditor consumes --dump-post (raw mesh +
# PBR volume) and does its own baking, so the WebP GLB texture path (and its
# libwebp FetchContent) is dead weight here.

if(TARGET qtmesh_trelliscpp OR NOT ENABLE_TRELLIS_CPP)
    return()
endif()

include(ExternalProject)

set(_trellis_exe_name trellis-cli)
if(WIN32)
    set(_trellis_exe_name trellis-cli.exe)
endif()
set(QTMESH_TRELLIS_CLI_BINARY
    "${CMAKE_BINARY_DIR}/_deps/qtmesh_trelliscpp-build/${_trellis_exe_name}"
    CACHE FILEPATH "Path of the bundled trellis-cli executable" FORCE)

set(_trellis_cmake_args
    -DCMAKE_BUILD_TYPE=Release
    -DTRELLIS_WEBP=OFF
    # Portable CPU code, not -mcpu/-march=native: this binary SHIPS — a
    # native-tuned CI build would crash on older machines, and Apple clang
    # rejects -mcpu=native when an explicit target arch is set anyway.
    -DGGML_NATIVE=OFF
    # Self-contained CLI for .deb / snap / .app — see header comment.
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
if(CMAKE_OSX_ARCHITECTURES)
    # LIST_SEPARATOR '|' below keeps a multi-arch value ("arm64;x86_64") as
    # ONE child argument instead of splitting at the semicolon.
    string(REPLACE ";" "|" _trellis_osx_archs "${CMAKE_OSX_ARCHITECTURES}")
    list(APPEND _trellis_cmake_args
         "-DCMAKE_OSX_ARCHITECTURES=${_trellis_osx_archs}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    # The release build targets macOS 11.0 — the child must match or the
    # bundled runtime could fail on supported systems.
    list(APPEND _trellis_cmake_args
         -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()

ExternalProject_Add(qtmesh_trelliscpp
    GIT_REPOSITORY https://github.com/pwilkin/trellis.cpp.git
    GIT_TAG        16f3109e82f3922033bfa62b83c42899678b7b6f
    GIT_SHALLOW    OFF
    PREFIX         "${CMAKE_BINARY_DIR}/_deps/qtmesh_trelliscpp"
    SOURCE_DIR     "${CMAKE_BINARY_DIR}/_deps/qtmesh_trelliscpp-src"
    BINARY_DIR     "${CMAKE_BINARY_DIR}/_deps/qtmesh_trelliscpp-build"
    LIST_SEPARATOR |
    CMAKE_ARGS     ${_trellis_cmake_args}
    BUILD_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --target trellis-cli
                   --config Release -j4
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${QTMESH_TRELLIS_CLI_BINARY}"
    UPDATE_DISCONNECTED ON)
