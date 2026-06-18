# ONNX Runtime for AI PBR map synthesis (#404).
#
# Downloads the official prebuilt ONNX Runtime release archive per-platform and
# exposes it as an imported SHARED target `qtmesh_onnx`. Mirrors the download
# pattern of cmake/Libsodium.cmake, but consumes a prebuilt binary instead of
# building from source.
#
# macOS uses the universal2 archive (covers arm64 + x86_64) so there is no
# per-arch selection trap (the lesson from libsodium being built x86_64). CoreML
# execution provider ships inside that archive; the CPU EP is always present.
# Windows MinGW is intentionally NOT wired here — the official Windows archive is
# MSVC-built and won't link under MinGW; that path degrades gracefully (the
# feature reports "rebuild with -DENABLE_ONNX"). See the #404 follow-up.

if(TARGET qtmesh_onnx)
    return()
endif()

set(QTMESH_ONNX_VERSION "1.20.1" CACHE STRING "ONNX Runtime release version")
set(QTMESH_ONNX_BASE_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v${QTMESH_ONNX_VERSION}")

# Select the archive + its SHA256 for this platform.
if(APPLE)
    set(_ort_archive "onnxruntime-osx-universal2-${QTMESH_ONNX_VERSION}.tgz")
    set(_ort_sha256  "da4349e01a7e997f5034563183c7183d069caadc1d95f499b560961787813efd")
    set(_ort_libname "libonnxruntime.${QTMESH_ONNX_VERSION}.dylib")
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_ort_archive "onnxruntime-linux-aarch64-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "ae4fedbdc8c18d688c01306b4b50c63de3445cdf2dbd720e01a2fa3810b8106a")
    else()
        set(_ort_archive "onnxruntime-linux-x64-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")
    endif()
    set(_ort_libname "libonnxruntime.so.${QTMESH_ONNX_VERSION}")
elseif(WIN32)
    set(_ort_archive "onnxruntime-win-x64-${QTMESH_ONNX_VERSION}.zip")
    set(_ort_sha256  "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581")
    set(_ort_libname "onnxruntime.dll")
else()
    message(FATAL_ERROR "ENABLE_ONNX: unsupported platform — no ONNX Runtime archive mapping. "
                        "Disable with -DENABLE_ONNX=OFF or add the archive here.")
endif()

include(FetchContent)
FetchContent_Declare(
    qtmesh_onnxruntime
    URL "${QTMESH_ONNX_BASE_URL}/${_ort_archive}"
    URL_HASH SHA256=${_ort_sha256}
)
FetchContent_MakeAvailable(qtmesh_onnxruntime)

# The archive extracts to a single top-level dir with include/ and lib/.
set(QTMESH_ONNX_ROOT "${qtmesh_onnxruntime_SOURCE_DIR}")
set(QTMESH_ONNX_INCLUDE_DIR "${QTMESH_ONNX_ROOT}/include")

# Resolve the actual shared-lib path. Prebuilt layouts vary slightly across
# platforms (versioned symlinks on *nix, lib/*.dll on Windows), so glob for it
# rather than hardcoding a single name.
file(GLOB _ort_libs
    "${QTMESH_ONNX_ROOT}/lib/${_ort_libname}"
    "${QTMESH_ONNX_ROOT}/lib/libonnxruntime*.dylib"
    "${QTMESH_ONNX_ROOT}/lib/libonnxruntime.so*"
    "${QTMESH_ONNX_ROOT}/lib/onnxruntime.dll")
if(NOT _ort_libs)
    message(FATAL_ERROR "ENABLE_ONNX: could not locate the ONNX Runtime shared library "
                        "under ${QTMESH_ONNX_ROOT}/lib")
endif()
list(GET _ort_libs 0 QTMESH_ONNX_RUNTIME_LIB)
set(QTMESH_ONNX_RUNTIME_LIB "${QTMESH_ONNX_RUNTIME_LIB}"
    CACHE FILEPATH "Path to the ONNX Runtime shared library to ship next to the binary" FORCE)

add_library(qtmesh_onnx SHARED IMPORTED GLOBAL)
set_target_properties(qtmesh_onnx PROPERTIES
    IMPORTED_LOCATION "${QTMESH_ONNX_RUNTIME_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${QTMESH_ONNX_INCLUDE_DIR}")
if(WIN32)
    # On Windows the import library is needed for linking.
    file(GLOB _ort_implib "${QTMESH_ONNX_ROOT}/lib/onnxruntime.lib")
    if(_ort_implib)
        list(GET _ort_implib 0 _ort_implib0)
        set_target_properties(qtmesh_onnx PROPERTIES IMPORTED_IMPLIB "${_ort_implib0}")
    endif()
endif()

message(STATUS "ONNX Runtime ${QTMESH_ONNX_VERSION}: ${QTMESH_ONNX_RUNTIME_LIB}")
