# Bundled trellis.cpp runtime (TRELLIS.2 image-to-3D, MIT code + weights).
#
# Builds pwilkin/trellis.cpp's `trellis-cli` as part of the QtMeshEditor build
# and ships it next to the editor binary, so TRELLIS.2 needs NO separate
# runtime install — only the GGUF weights, which AI Model Settings downloads
# into <AppData>/ai_models/trellis2/ (a directory Trellis2Predictor already
# resolves). MUST include PR #45 (`--dump-post`) — 3.37.6 shipped pin
# 16f3109e (2026-08-21) which predated that merge and every snap/deb invoke
# failed with "unknown option: --dump-post". Current pin is upstream main
# after #44 (Metal) + #45 (dump-post). On Linux x86_64 also force
# AVX-without-FMA (see GGML_* flags below) — 3.37.7's Haswell default
# SIGILL'd on Ivy Bridge. Re-audit licenses if the pin moves
# (docs/trellis2-dependencies.md).
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
# GGML_NATIVE=OFF alone is NOT portable on x86_64: ggml then defaults
# INS_ENB=ON (AVX2+FMA+F16C+BMI2) — Haswell+. 3.37.7 snap SIGILL'd
# (vfmadd213ss / exit 4) on Ivy Bridge (AVX+F16C, no FMA). Pin an
# AVX+SSE4.2 baseline without FMA/AVX2/F16C/BMI2 so Sandy Bridge+.
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    list(APPEND _trellis_cmake_args
         -DGGML_SSE42=ON
         -DGGML_AVX=ON
         -DGGML_AVX2=OFF
         -DGGML_FMA=OFF
         -DGGML_F16C=OFF
         -DGGML_BMI2=OFF)
endif()
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
    GIT_TAG        2516c48b677050c570f47eba2e68dc8a5bc918b0
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
