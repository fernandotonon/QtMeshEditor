# ONNX Runtime for AI PBR map synthesis (#404).
#
# Downloads the official prebuilt ONNX Runtime release archive per-platform and
# exposes it as an imported SHARED target `qtmesh_onnx`. Mirrors the download
# pattern of cmake/Libsodium.cmake, but consumes a prebuilt binary instead of
# building from source.
#
# macOS arm64 uses the osx-arm64 archive. ONNX Runtime stopped publishing the
# universal2 / x86_64 macOS archives after 1.23.2, so an Intel or universal
# macOS build stays on the 1.23.2 universal2 archive (QTMESH_ONNX_MAC_LEGACY_VERSION).
# The CoreML execution provider ships inside both; the CPU EP is always present.
# 1.31 runs the same graphs markedly faster on Arm (Kimodo denoiser: 226 -> 88 ms/call).
#
# Linux x64 GPU: pass -DQTMESH_ONNX_GPU=ON (auto-defaults ON when nvidia-smi is
# found outside CI) to fetch onnxruntime-linux-x64-gpu_cuda12-*.tgz. The CUDA provider
# .so must ship next to the binary AND cuDNN 9 + CUDA 12 must be on the system.
# Windows (MSVC and MinGW) uses the official win-x64 archive. The DLL exports a
# plain C ABI (x64: no stdcall decoration), so MinGW links it too — via the
# shipped onnxruntime.lib import library, which GNU binutils reads (short-
# import-archive format). NB the import library, NOT the DLL directly: the CI
# link runs with -static, and GNU ld refuses raw dynamic objects in static
# mode while import libraries are ordinary archives (the same way the Qt
# .dll.a import libs link there).

if(TARGET qtmesh_onnx)
    return()
endif()

# Not a cache variable: the archive digests below belong to exactly this version, and a
# cached value would survive an upgrade in an existing build directory (old archive +
# new digest -> FetchContent verification failure). Drop any entry an older configure left.
unset(QTMESH_ONNX_VERSION CACHE)
set(QTMESH_ONNX_VERSION "1.31.1")
set(QTMESH_ONNX_MAC_LEGACY_VERSION "1.23.2")
set(QTMESH_ONNX_BASE_URL
    "https://github.com/microsoft/onnxruntime/releases/download/v${QTMESH_ONNX_VERSION}")

# Default GPU package on local Linux x64 builds when an NVIDIA GPU is present.
set(_qtmesh_onnx_gpu_default OFF)
if(UNIX AND NOT APPLE AND NOT DEFINED ENV{CI})
    find_program(_QTMESH_NVIDIA_SMI nvidia-smi)
    if(_QTMESH_NVIDIA_SMI)
        set(_qtmesh_onnx_gpu_default ON)
    endif()
endif()
option(QTMESH_ONNX_GPU
    "Download GPU ONNX Runtime (CUDA on Linux x64; adds ~700 MB provider libs)"
    ${_qtmesh_onnx_gpu_default})

# Select the archive + its SHA256 for this platform.
if(APPLE)
    set(_ort_fetch_name "qtmesh_onnxruntime")
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"
       OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
        set(_ort_archive "onnxruntime-osx-arm64-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "2e1344a1926ee50eb25b38153cb6992c3567515aef0e677a1a520a1798896d6f")
    else()
        message(STATUS "ONNX Runtime: no ${QTMESH_ONNX_VERSION} archive for Intel/universal macOS; "
                       "using ${QTMESH_ONNX_MAC_LEGACY_VERSION} universal2")
        set(QTMESH_ONNX_VERSION "${QTMESH_ONNX_MAC_LEGACY_VERSION}")
        set(QTMESH_ONNX_BASE_URL
            "https://github.com/microsoft/onnxruntime/releases/download/v${QTMESH_ONNX_VERSION}")
        set(_ort_archive "onnxruntime-osx-universal2-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "49ae8e3a66ccb18d98ad3fe7f5906b6d7887df8a5edd40f49eb2b14e20885809")
        set(_ort_fetch_name "qtmesh_onnxruntime_universal2")
    endif()
    set(_ort_libname "libonnxruntime.${QTMESH_ONNX_VERSION}.dylib")
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        if(QTMESH_ONNX_GPU)
            message(STATUS "QTMESH_ONNX_GPU: no GPU ONNX archive for Linux aarch64; using CPU ONNX Runtime")
            set(QTMESH_ONNX_GPU OFF CACHE BOOL "" FORCE)
        endif()
        set(_ort_archive "onnxruntime-linux-aarch64-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "5317505e2da99be65f8e2775ba3c2afd6d2961fd78aba1bd8f56159f48820c78")
        set(_ort_fetch_name "qtmesh_onnxruntime")
    else()
        if(QTMESH_ONNX_GPU)
            set(_ort_archive "onnxruntime-linux-x64-gpu_cuda12-${QTMESH_ONNX_VERSION}.tgz")
            set(_ort_sha256  "57cd51f677eca19a269cdba31b0c16c15d73a8feea41f170aa8b750447ab1f70")
            set(_ort_fetch_name "qtmesh_onnxruntime_gpu")
        else()
            set(_ort_archive "onnxruntime-linux-x64-${QTMESH_ONNX_VERSION}.tgz")
            set(_ort_sha256  "c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9")
            set(_ort_fetch_name "qtmesh_onnxruntime")
        endif()
    endif()
    set(_ort_libname "libonnxruntime.so.${QTMESH_ONNX_VERSION}")
elseif(WIN32)
    if(QTMESH_ONNX_GPU)
        message(WARNING "QTMESH_ONNX_GPU: Windows GPU package not wired in CMake yet; using CPU ONNX Runtime")
        set(QTMESH_ONNX_GPU OFF CACHE BOOL "" FORCE)
    endif()
    set(_ort_archive "onnxruntime-win-x64-${QTMESH_ONNX_VERSION}.zip")
    set(_ort_sha256  "5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c")
    set(_ort_fetch_name "qtmesh_onnxruntime")
    set(_ort_libname "onnxruntime.dll")
else()
    message(FATAL_ERROR "ENABLE_ONNX: unsupported platform — no ONNX Runtime archive mapping. "
                        "Disable with -DENABLE_ONNX=OFF or add the archive here.")
endif()

include(FetchContent)
FetchContent_Declare(
    ${_ort_fetch_name}
    URL "${QTMESH_ONNX_BASE_URL}/${_ort_archive}"
    URL_HASH SHA256=${_ort_sha256}
)
FetchContent_MakeAvailable(${_ort_fetch_name})

# FetchContent sets <name>_SOURCE_DIR
set(QTMESH_ONNX_ROOT "${${_ort_fetch_name}_SOURCE_DIR}")
set(QTMESH_ONNX_INCLUDE_DIR "${QTMESH_ONNX_ROOT}/include")

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
get_filename_component(QTMESH_ONNX_LIB_DIR "${QTMESH_ONNX_RUNTIME_LIB}" DIRECTORY)
set(QTMESH_ONNX_LIB_DIR "${QTMESH_ONNX_LIB_DIR}"
    CACHE PATH "Directory of the ONNX Runtime shared library + provider libs" FORCE)

set(QTMESH_ONNX_GPU "${QTMESH_ONNX_GPU}" CACHE BOOL
    "Using GPU ONNX Runtime package (CUDA/DirectML EPs)" FORCE)

add_library(qtmesh_onnx SHARED IMPORTED GLOBAL)
set_target_properties(qtmesh_onnx PROPERTIES
    IMPORTED_LOCATION "${QTMESH_ONNX_RUNTIME_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${QTMESH_ONNX_INCLUDE_DIR}")
if(WIN32)
    if(MINGW)
        # MinGW-w64's sal.h lags the newer MSVC source annotations ORT's
        # header uses (_Frees_ptr_opt_ & co) — force-include a shim that
        # no-op defines only what the real sal.h did not provide.
        set_property(TARGET qtmesh_onnx APPEND PROPERTY
            INTERFACE_COMPILE_OPTIONS
            "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/onnx_mingw_sal_shim.h")
    endif()
    # Both toolchains use the shipped import library (see the header comment:
    # under -static, GNU ld accepts archives but refuses raw DLLs).
    file(GLOB _ort_implib "${QTMESH_ONNX_ROOT}/lib/onnxruntime.lib")
    if(_ort_implib)
        list(GET _ort_implib 0 _ort_implib0)
        set_target_properties(qtmesh_onnx PROPERTIES IMPORTED_IMPLIB "${_ort_implib0}")
    elseif(MINGW)
        message(FATAL_ERROR "ENABLE_ONNX: onnxruntime.lib missing from the win-x64 "
                            "archive — MinGW needs the import library to link.")
    endif()
endif()

if(QTMESH_ONNX_GPU)
    message(STATUS "ONNX Runtime ${QTMESH_ONNX_VERSION} (GPU): ${QTMESH_ONNX_RUNTIME_LIB}")
else()
    message(STATUS "ONNX Runtime ${QTMESH_ONNX_VERSION} (CPU): ${QTMESH_ONNX_RUNTIME_LIB}")
endif()
