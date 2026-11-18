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

# Where the archive comes from. GITHUB = the release archive (the historical source);
# NUGET = Microsoft's own Microsoft.ML.OnnxRuntime package from nuget.org, the same build
# laid out like the archive; AUTO = GitHub, then NuGet when the GitHub download fails.
# github.com/microsoft/onnxruntime went offline in November 2026 (404 for the repository
# and every release asset) while the NuGet package stayed up; both are SHA-256 pinned.
set(QTMESH_ONNX_SOURCE "AUTO" CACHE STRING "ONNX Runtime download source: AUTO, GITHUB or NUGET")
set_property(CACHE QTMESH_ONNX_SOURCE PROPERTY STRINGS AUTO GITHUB NUGET)
set(_ort_nuget_rid "")   # set below for the platforms the NuGet package covers
set(_ort_nuget_sha256 "6871ccbc97f3d5bdb4095a1dcec33aa78d56f6ed8de5980ef5fa1d9f2e7c679a")

# Select the archive + its SHA256 for this platform.
if(APPLE)
    set(_ort_fetch_name "qtmesh_onnxruntime")
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"
       OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
        set(_ort_archive "onnxruntime-osx-arm64-${QTMESH_ONNX_VERSION}.tgz")
        set(_ort_sha256  "2e1344a1926ee50eb25b38153cb6992c3567515aef0e677a1a520a1798896d6f")
        set(_ort_nuget_rid "osx-arm64")
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
        set(_ort_nuget_rid "linux-arm64")
        set(_ort_fetch_name "qtmesh_onnxruntime")
    else()
        if(QTMESH_ONNX_GPU)
            set(_ort_archive "onnxruntime-linux-x64-gpu_cuda12-${QTMESH_ONNX_VERSION}.tgz")
            set(_ort_sha256  "57cd51f677eca19a269cdba31b0c16c15d73a8feea41f170aa8b750447ab1f70")
            set(_ort_fetch_name "qtmesh_onnxruntime_gpu")
        else()
            set(_ort_archive "onnxruntime-linux-x64-${QTMESH_ONNX_VERSION}.tgz")
            set(_ort_sha256  "c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9")
            set(_ort_nuget_rid "linux-x64")
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
    set(_ort_nuget_rid "win-x64")
    set(_ort_fetch_name "qtmesh_onnxruntime")
    set(_ort_libname "onnxruntime.dll")
else()
    message(FATAL_ERROR "ENABLE_ONNX: unsupported platform — no ONNX Runtime archive mapping. "
                        "Disable with -DENABLE_ONNX=OFF or add the archive here.")
endif()

include(FetchContent)

# Downloads `url` to `out` and checks its SHA-256. Sets `ok_var` to TRUE on success, FALSE
# when the download failed; a file that arrives with the WRONG digest is a hard error (never
# silently fall back past a tampered or changed archive). Transfer errors other than an
# HTTP error status (curl 22, e.g. the 404) are retried, and a stalled transfer times out.
function(_qtmesh_ort_download url out sha256 ok_var)
    if(EXISTS "${out}")
        file(SHA256 "${out}" _have)
        if(_have STREQUAL sha256)
            set(${ok_var} TRUE PARENT_SCOPE)
            return()
        endif()
        file(REMOVE "${out}")
    endif()
    foreach(_attempt RANGE 1 3)
        message(STATUS "ONNX Runtime: downloading ${url} (attempt ${_attempt})")
        file(DOWNLOAD "${url}" "${out}.part" STATUS _status TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
        list(GET _status 0 _code)
        if(_code EQUAL 0)
            break()
        endif()
        list(GET _status 1 _msg)
        message(STATUS "ONNX Runtime: download failed (${_code}: ${_msg})")
        file(REMOVE "${out}.part")
        if(_code EQUAL 22)
            break()
        endif()
    endforeach()
    if(NOT _code EQUAL 0)
        set(${ok_var} FALSE PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${out}.part" _have)
    if(NOT _have STREQUAL sha256)
        file(REMOVE "${out}.part")
        message(FATAL_ERROR "ONNX Runtime: SHA-256 mismatch for ${url}\n"
                            "  expected ${sha256}\n  got      ${_have}")
    endif()
    file(RENAME "${out}.part" "${out}")
    set(${ok_var} TRUE PARENT_SCOPE)
endfunction()

# Lays the NuGet package out like the release archive (include/ + lib/ with the same file
# names, including the SONAME links), so everything below and the packaging globs in
# src/CMakeLists.txt work unchanged. The libraries in the package carry the same install
# names as the archive's (@rpath/libonnxruntime.1.dylib, libonnxruntime.so.1).
function(_qtmesh_ort_stage_nuget nupkg rid root)
    set(_v "${QTMESH_ONNX_VERSION}")
    if(rid MATCHES "^osx")
        set(_main "libonnxruntime.${_v}.dylib")
    elseif(rid MATCHES "^linux")
        set(_main "libonnxruntime.so.${_v}")
    else()
        set(_main "onnxruntime.dll")
    endif()
    # The stamp alone is not trusted: a partial cleanup can leave it behind.
    set(_stamp "${root}/.qtmesh-ort-${_v}-${rid}")
    if(EXISTS "${_stamp}" AND EXISTS "${root}/lib/${_main}"
       AND EXISTS "${root}/include/onnxruntime_cxx_api.h")
        return()
    endif()
    file(REMOVE_RECURSE "${root}" "${root}-x")
    file(ARCHIVE_EXTRACT INPUT "${nupkg}" DESTINATION "${root}-x"
         PATTERNS "build/native/include/*" "runtimes/${rid}/native/*")
    file(MAKE_DIRECTORY "${root}/lib")
    file(COPY "${root}-x/build/native/include/" DESTINATION "${root}/include")
    set(_native "${root}-x/runtimes/${rid}/native")
    if(rid MATCHES "^osx")
        file(COPY_FILE "${_native}/libonnxruntime.dylib" "${root}/lib/${_main}")
        file(CREATE_LINK "${_main}" "${root}/lib/libonnxruntime.1.dylib" SYMBOLIC)
        file(CREATE_LINK "${_main}" "${root}/lib/libonnxruntime.dylib" SYMBOLIC)
    elseif(rid MATCHES "^linux")
        file(COPY_FILE "${_native}/libonnxruntime.so" "${root}/lib/${_main}")
        file(CREATE_LINK "${_main}" "${root}/lib/libonnxruntime.so.1" SYMBOLIC)
        file(CREATE_LINK "libonnxruntime.so.1" "${root}/lib/libonnxruntime.so" SYMBOLIC)
        if(EXISTS "${_native}/libonnxruntime_providers_shared.so")
            file(COPY_FILE "${_native}/libonnxruntime_providers_shared.so"
                           "${root}/lib/libonnxruntime_providers_shared.so")
        endif()
    else()
        file(GLOB _win_files "${_native}/*")
        file(COPY ${_win_files} DESTINATION "${root}/lib")
    endif()
    file(REMOVE_RECURSE "${root}-x")
    file(TOUCH "${_stamp}")
endfunction()

string(TOUPPER "${QTMESH_ONNX_SOURCE}" _ort_source)
if(NOT _ort_source MATCHES "^(AUTO|GITHUB|NUGET)$")
    message(FATAL_ERROR "QTMESH_ONNX_SOURCE must be AUTO, GITHUB or NUGET (got '${QTMESH_ONNX_SOURCE}')")
endif()
if(NOT _ort_nuget_rid OR NOT QTMESH_ONNX_VERSION STREQUAL "1.31.1")
    # The pinned package digest belongs to 1.31.1, and the package has no universal2 or
    # CUDA build: those configurations can only use the GitHub archive.
    if(_ort_source STREQUAL "NUGET")
        message(FATAL_ERROR "QTMESH_ONNX_SOURCE=NUGET: no NuGet mapping for ${_ort_archive}")
    endif()
    set(_ort_nuget_rid "")
endif()

# A local source override (FETCHCONTENT_SOURCE_DIR_<NAME>) or a disconnected configure keeps
# the plain FetchContent behaviour: no download here, FetchContent applies its own rules.
string(TOUPPER "${_ort_fetch_name}" _ort_fetch_upper)
set(_ort_fetch_local FALSE)
if(DEFINED FETCHCONTENT_SOURCE_DIR_${_ort_fetch_upper} OR FETCHCONTENT_FULLY_DISCONNECTED)
    set(_ort_fetch_local TRUE)
endif()

set(_ort_dl_dir "${FETCHCONTENT_BASE_DIR}/qtmesh_onnxruntime_downloads")
set(_ort_local_archive "${_ort_dl_dir}/${_ort_archive}")
set(_ort_have_archive FALSE)
if(NOT _ort_fetch_local AND NOT _ort_source STREQUAL "NUGET")
    file(MAKE_DIRECTORY "${_ort_dl_dir}")
    # A build directory configured before this fallback existed already holds the archive
    # FetchContent downloaded: reuse it instead of going back to the network.
    set(_ort_old_copy
        "${FETCHCONTENT_BASE_DIR}/${_ort_fetch_name}-subbuild/${_ort_fetch_name}-populate-prefix/src/${_ort_archive}")
    if(NOT EXISTS "${_ort_local_archive}" AND EXISTS "${_ort_old_copy}")
        file(COPY_FILE "${_ort_old_copy}" "${_ort_local_archive}")
    endif()
    _qtmesh_ort_download("${QTMESH_ONNX_BASE_URL}/${_ort_archive}" "${_ort_local_archive}"
                         "${_ort_sha256}" _ort_have_archive)
endif()

if(_ort_fetch_local OR _ort_have_archive)
    if(_ort_have_archive)
        set(_ort_url "${_ort_local_archive}")
    else()
        set(_ort_url "${QTMESH_ONNX_BASE_URL}/${_ort_archive}")
    endif()
    FetchContent_Declare(
        ${_ort_fetch_name}
        URL "${_ort_url}"
        URL_HASH SHA256=${_ort_sha256}
    )
    FetchContent_MakeAvailable(${_ort_fetch_name})
    # FetchContent sets <name>_SOURCE_DIR
    set(QTMESH_ONNX_ROOT "${${_ort_fetch_name}_SOURCE_DIR}")
elseif(_ort_nuget_rid AND NOT _ort_source STREQUAL "GITHUB")
    file(MAKE_DIRECTORY "${_ort_dl_dir}")
    set(_ort_nupkg "${_ort_dl_dir}/microsoft.ml.onnxruntime.${QTMESH_ONNX_VERSION}.nupkg")
    _qtmesh_ort_download(
        "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${QTMESH_ONNX_VERSION}/microsoft.ml.onnxruntime.${QTMESH_ONNX_VERSION}.nupkg"
        "${_ort_nupkg}" "${_ort_nuget_sha256}" _ort_have_nupkg)
    if(NOT _ort_have_nupkg)
        message(FATAL_ERROR "ONNX Runtime: the NuGet package could not be downloaded"
                            " (and the GitHub archive was not tried or failed)."
                            " Disable with -DENABLE_ONNX=OFF.")
    endif()
    set(QTMESH_ONNX_ROOT "${FETCHCONTENT_BASE_DIR}/qtmesh_onnxruntime_nuget_${_ort_nuget_rid}")
    _qtmesh_ort_stage_nuget("${_ort_nupkg}" "${_ort_nuget_rid}" "${QTMESH_ONNX_ROOT}")
    message(STATUS "ONNX Runtime: using the NuGet package (${_ort_nuget_rid})")
else()
    message(FATAL_ERROR "ONNX Runtime: could not download ${QTMESH_ONNX_BASE_URL}/${_ort_archive}"
                        " and QTMESH_ONNX_SOURCE=${_ort_source} / this configuration allows no"
                        " NuGet fallback. Disable with -DENABLE_ONNX=OFF.")
endif()
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
