#############################################################
#
# On Windows, define the following environnemental variables before running:
#
# - OGRE_HOME (C:\OgreSDK)
# - CMAKE_PREFIX_PATH (to Qt <version>\<compiler>\lib\cmake folder)
#
##############################################################
#  Versions
##############################################################

cmake_minimum_required(VERSION 3.24.0)
cmake_policy(SET CMP0005 NEW)
cmake_policy(SET CMP0048 NEW) # manages project version

project(QtMeshEditor VERSION 3.27.0 LANGUAGES C CXX)
message(STATUS "Building QtMeshEditor version ${PROJECT_VERSION}")

set(QTMESHEDITOR_VERSION_STRING "\"${PROJECT_VERSION}\"")
if(WIN32 OR APPLE)
set(PLUGIN_DIR "../")
endif()

add_definitions( -DQTMESHEDITOR_VERSION=${QTMESHEDITOR_VERSION_STRING} )

# Public QtMesh Cloud web URL (used by CLI promo + any future link-outs).
# Override with -DQTMESH_CLOUD_WEB_URL=https://... for self-hosted instances.
set(QTMESH_CLOUD_WEB_URL "https://qtmesh.dev" CACHE STRING
    "QtMesh Cloud web UI base URL (no trailing slash)")
# Normalize: strip a single trailing slash so callers can paste a copied URL
# without breaking string concatenation downstream.
string(REGEX REPLACE "/+$" "" QTMESH_CLOUD_WEB_URL "${QTMESH_CLOUD_WEB_URL}")
add_definitions(-DQTMESH_CLOUD_WEB_URL="${QTMESH_CLOUD_WEB_URL}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

##############################################################
#  Configuring CMake
##############################################################
if(WIN32)
    cmake_policy(SET CMP0020 NEW)   # to avoid cmake warning
endif()

enable_language(CXX)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
# Building directories
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "debug")
    MESSAGE("DEBUG COMPILATION")
    # NOTE: do NOT use ADD_DEFINITIONS("-DDEBUG") here — it's directory-scoped
    # and leaks into the FetchContent'd llama.cpp/mtmd subdir, where
    # mtmd-audio.cpp uses DEBUG as an ordinary identifier and fails to compile.
    # Apply -DDEBUG to OUR targets only, via this marker + target_compile_
    # definitions on the app/test targets (see src/CMakeLists.txt).
    set(QTMESH_DEFINE_DEBUG ON)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib-debug)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib-debug)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  ${CMAKE_BINARY_DIR}/debug)
    set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/debug)
    set(BUILD_INCLUDE_DIR  ${CMAKE_BINARY_DIR}/include_d)
else()
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: None (CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel." FORCE)
    ADD_DEFINITIONS(-DQT_NO_DEBUG -DQT_NO_DEBUG_OUTPUT -DQT_NO_WARNING_OUTPUT)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/bin)
    set(BUILD_INCLUDE_DIR  ${CMAKE_BINARY_DIR}/include)
endif()

# Instruct CMake to run moc automatically when needed.
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

##############################################################
#  Searching Qt dependencies
##############################################################
# On newer macOS (11+), AGL framework is deprecated and has no actual library.
# Create a stub AGL.framework for linking purposes
if(APPLE)
    set(AGL_STUB_DIR "${CMAKE_BINARY_DIR}/AGL_stub")
    set(AGL_FW_DIR "${AGL_STUB_DIR}/AGL.framework")
    set(AGL_FW_LIB "${AGL_FW_DIR}/Versions/A/AGL")

    if(NOT EXISTS "${AGL_FW_LIB}")
        file(MAKE_DIRECTORY "${AGL_FW_DIR}/Versions/A")
        file(WRITE "${AGL_STUB_DIR}/agl_stub.c" "void __agl_stub_placeholder(void) {}")
        # Build for arm64 since that's what modern macOS uses
        execute_process(
            COMMAND clang -arch arm64 -dynamiclib
                -o "${AGL_FW_LIB}"
                "${AGL_STUB_DIR}/agl_stub.c"
                -install_name "/System/Library/Frameworks/AGL.framework/Versions/A/AGL"
                -current_version 1.0.0 -compatibility_version 1.0.0
            WORKING_DIRECTORY "${AGL_STUB_DIR}"
        )
        # Create symlinks
        execute_process(COMMAND ln -sf A "${AGL_FW_DIR}/Versions/Current")
        execute_process(COMMAND ln -sf Versions/Current/AGL "${AGL_FW_DIR}/AGL")
    endif()

    # Add our stub framework directory to the framework search path
    link_directories("${AGL_STUB_DIR}")
    set(CMAKE_FRAMEWORK_PATH "${AGL_STUB_DIR}" ${CMAKE_FRAMEWORK_PATH})
endif()

#Find Qt Packages
find_package(QT NAMES Qt6)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui QuickWidgets Quick Qml QmlModels QmlMeta Test Network QuickControls2)

message("Found Qt (${QT_VERSION_MAJOR})")

SET (QTLIBLIST
    Qt${QT_VERSION_MAJOR}Gui
    Qt${QT_VERSION_MAJOR}Core
    Qt${QT_VERSION_MAJOR}Widgets
    Qt${QT_VERSION_MAJOR}QuickWidgets
    Qt${QT_VERSION_MAJOR}Quick
    Qt${QT_VERSION_MAJOR}Qml
    Qt${QT_VERSION_MAJOR}QmlModels
    Qt${QT_VERSION_MAJOR}QmlWorkerScript
    Qt${QT_VERSION_MAJOR}Network
    Qt${QT_VERSION_MAJOR}OpenGL
    Qt${QT_VERSION_MAJOR}QmlMeta
    Qt${QT_VERSION_MAJOR}QuickControls2
    Qt${QT_VERSION_MAJOR}QuickTemplates2
)
FOREACH(qtlib ${QTLIBLIST})
        include_directories(${${qtlib}_INCLUDE_DIRS})
ENDFOREACH(qtlib)

#set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/;${CMAKE_MODULE_PATH}")

##############################################################
#  googletest
##############################################################
option(BUILD_TESTS "Build the unity tests" OFF)

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/f8d7d77c06936315286eb55f8de22cd23c188571.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # Include Google Test headers
    include_directories(
        ${gtest_SOURCE_DIR}/include
        ${gtest_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    enable_testing()

    # Add the test
    add_test(NAME UnitTests COMMAND UnitTests)
    include(GoogleTest)
endif()

##############################################################
#  llama.cpp for local LLM inference
##############################################################
option(ENABLE_LOCAL_LLM "Enable local LLM inference with llama.cpp" ON)

if(ENABLE_LOCAL_LLM)
    include(FetchContent)

    # Configure llama.cpp build options
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # The `mtmd` multimodal library (tools/mtmd) powers image captioning
    # (SmolVLM, #764). It only builds under LLAMA_BUILD_TOOLS + LLAMA_BUILD_COMMON
    # — but enabling all llama tools also builds CLI executables
    # (cvector-generator, export-lora, llama-*-cli) that FAIL TO LINK on
    # Windows-MinGW, where GGML_CPU is off (undefined ggml_backend_cpu_init /
    # ggml_get_f32_nd). We don't ship those tools. So enable them only where
    # they link: NOT on MinGW. The captioner (QTMESH_HAVE_MTMD) degrades
    # gracefully when absent, and Windows-MinGW has ONNX off anyway, so the
    # image-to-3D texture path there already falls back.
    if(WIN32 AND MINGW)
        set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(QTMESH_HAVE_MTMD OFF)
    else()
        set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS ON CACHE BOOL "" FORCE)
        set(QTMESH_HAVE_MTMD ON)
    endif()

    # Enable Metal on macOS for GPU acceleration
    if(APPLE)
        set(GGML_METAL ON CACHE BOOL "" FORCE)
        set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
        message(STATUS "Enabling Metal acceleration for llama.cpp on macOS")
    endif()

    # Enable CUDA on systems with NVIDIA GPUs
    if(CMAKE_CUDA_COMPILER OR DEFINED ENV{CUDA_PATH})
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
        message(STATUS "Enabling CUDA acceleration for llama.cpp")
    endif()

    # Disable native optimizations for cross-platform compatibility
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    
    # Enable CUDA for NVIDIA GPUs (much faster and avoids CPU compatibility issues)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
        message(STATUS "CUDA found - enabling GPU acceleration for llama.cpp")
    endif()
    
    # For CPU fallback: disable AVX2/FMA for older CPUs (pre-Haswell compatibility)
    set(GGML_AVX2 OFF CACHE BOOL "" FORCE)
    set(GGML_FMA OFF CACHE BOOL "" FORCE)
    set(GGML_BMI2 OFF CACHE BOOL "" FORCE)

    # On Windows with MinGW, disable CPU backend to avoid SDK compatibility issues
    if(WIN32 AND MINGW)
        set(GGML_CPU OFF CACHE BOOL "" FORCE)
        message(STATUS "Disabling CPU backend on MinGW due to SDK compatibility")
    endif()

    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG b9267
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(llama_cpp)

    # Include llama.cpp headers (+ mtmd multimodal for image captioning where
    # available — not on Windows-MinGW, see QTMESH_HAVE_MTMD above).
    include_directories(
        ${llama_cpp_SOURCE_DIR}/include
        ${llama_cpp_SOURCE_DIR}/ggml/include
    )
    if(QTMESH_HAVE_MTMD)
        include_directories(
            ${llama_cpp_SOURCE_DIR}/tools/mtmd
            ${llama_cpp_SOURCE_DIR}/common
        )
        add_definitions(-DENABLE_MTMD)
        message(STATUS "mtmd multimodal (image captioning) enabled")
    endif()

    add_definitions(-DENABLE_LOCAL_LLM)
    message(STATUS "Local LLM support enabled with llama.cpp")
endif()
##############################################################
#  stable-diffusion.cpp for AI texture generation
##############################################################
option(ENABLE_STABLE_DIFFUSION "Enable AI texture generation with stable-diffusion.cpp" OFF)

if(ENABLE_STABLE_DIFFUSION)
    include(FetchContent)

    # Configure stable-diffusion.cpp build options
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SD_BUILD_SERVER OFF CACHE BOOL "" FORCE)

    # Enable Metal on macOS for GPU acceleration
    if(APPLE)
        set(SD_METAL ON CACHE BOOL "" FORCE)
        message(STATUS "Enabling Metal acceleration for stable-diffusion.cpp on macOS")
    endif()

    # Enable CUDA on systems with NVIDIA GPUs
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(SD_CUDA ON CACHE BOOL "" FORCE)
        message(STATUS "CUDA found - enabling GPU acceleration for stable-diffusion.cpp")
    endif()

    FetchContent_Declare(
        stable_diffusion_cpp
        GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
        GIT_TAG 545fac4
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(stable_diffusion_cpp)

    # Include stable-diffusion.cpp headers
    include_directories(
        ${stable_diffusion_cpp_SOURCE_DIR}
    )

    add_definitions(-DENABLE_STABLE_DIFFUSION)
    message(STATUS "AI texture generation enabled with stable-diffusion.cpp")
endif()
##############################################################
#  ONNX Runtime — AI PBR map synthesis (#404)
##############################################################
option(ENABLE_ONNX "Enable AI PBR map synthesis via ONNX Runtime" OFF)

if(ENABLE_ONNX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OnnxRuntime.cmake)
    add_definitions(-DENABLE_ONNX)
    if(QTMESH_ONNX_GPU)
        add_definitions(-DQTMESH_ONNX_GPU_BUILD)
        message(STATUS "ONNX Runtime GPU package enabled (CUDA EP on Linux x64 when cuDNN 9 is installed)")
    endif()
    message(STATUS "AI PBR map synthesis enabled with ONNX Runtime")
endif()
##############################################################
#  Alembic — mesh / vertex animation import (#519, Anim Slice B)
##############################################################
# Vendored (BSD) reader for baked per-vertex caches (cloth / sims / fluids /
# Houdini + Blender exports). Default OFF: the Alembic + Imath build is
# non-trivial across the three target platforms. VertexAnimationManager + Ogre
# VAT_POSE playback work WITHOUT Alembic; this flag adds the .abc reader
# (cmake/Alembic.cmake, FetchContent). The C++ #ifdef ENABLE_ALEMBIC guards the
# reader so a build without it still compiles and skips .abc with a clear message.
option(ENABLE_ALEMBIC "Enable Alembic (.abc) vertex-animation import" OFF)

if(ENABLE_ALEMBIC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Alembic.cmake)
    add_definitions(-DENABLE_ALEMBIC)
    message(STATUS "Alembic vertex-animation import enabled")
else()
    message(STATUS "Alembic vertex-animation import disabled (VAT_POSE playback still available)")
endif()
##############################################################
#  Performance capture — video/webcam mocap (epic #869)
##############################################################
# Slice B (#871): brings in Qt Multimedia (camera + video decode) and the
# src/Mocap/ frame-source layer. Default OFF: Qt Multimedia is a new runtime
# dependency (FFmpeg backend) that packaging has to carry per platform. The
# ONNX predictors (Slices C/E) additionally require ENABLE_ONNX; requiring it
# here keeps a single "mocap build" configuration instead of a half-working
# one. Non-mocap builds print "rebuild with -DENABLE_MOCAP" on every surface.
option(ENABLE_MOCAP "Enable performance capture (video/webcam mocap)" OFF)

if(ENABLE_MOCAP)
    if(NOT ENABLE_ONNX)
        message(FATAL_ERROR "ENABLE_MOCAP requires ENABLE_ONNX (the face/pose "
                "predictors run on ONNX Runtime). Configure with "
                "-DENABLE_ONNX=ON -DENABLE_MOCAP=ON.")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Multimedia)
    add_definitions(-DENABLE_MOCAP)
    message(STATUS "Performance capture enabled (Qt Multimedia ${Qt6Multimedia_VERSION})")
else()
    message(STATUS "Performance capture disabled (rebuild with -DENABLE_MOCAP=ON -DENABLE_ONNX=ON)")
endif()
##############################################################
#  PS1 runtime geometry extraction (experimental)
##############################################################
option(ENABLE_PS1_RIP "Enable experimental PS1 runtime geometry extraction" OFF)

if(ENABLE_PS1_RIP)
    add_definitions(-DENABLE_PS1_RIP)
    message(STATUS "PS1 runtime geometry extraction enabled (experimental)")
    find_package(Qt6 COMPONENTS Gamepad QUIET)
    if(TARGET Qt6::Gamepad)
        set(QTMESH_HAVE_QT_GAMEPAD ON CACHE INTERNAL "")
        message(STATUS "Qt6 Gamepad found — PS1 controller support enabled")
    else()
        message(STATUS "Qt6 Gamepad not found — PS1 keyboard/mouse only")
    endif()
    add_subdirectory(plugins/ps1core_libretro)
    add_subdirectory(plugins/ps1core_stub)
endif()
##############################################################
#  Area lights (Slice I #491 — multi-point approximation)
##############################################################
option(ENABLE_AREA_LIGHTS "Enable area-light gizmo + multi-point approximation" ON)

if(ENABLE_AREA_LIGHTS)
    add_definitions(-DENABLE_AREA_LIGHTS)
    message(STATUS "Area lights enabled (multi-point approximation)")
endif()
##############################################################
#  In-app auto-updater (portable Windows/macOS/Linux tarballs)
##############################################################
option(ENABLE_AUTO_UPDATER "Enable in-app auto-updater UI and download path" ON)

if(ENABLE_AUTO_UPDATER)
    add_definitions(-DENABLE_AUTO_UPDATER)
    message(STATUS "In-app auto-updater enabled")
endif()
##############################################################
#  meshoptimizer — LOD generation, vertex-cache / overdraw /
#  fetch optimization, vertex remap dedupe. MIT, header-light,
#  ~200 KB. First consumer is `qtmesh lod --algo meshopt`
#  (issue #398 under the AI-Assist epic #397). Always built —
#  small enough that no opt-out flag is justified.
##############################################################
include(FetchContent)
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v0.21
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(meshoptimizer)
message(STATUS "meshoptimizer enabled (LOD, vertex cache, dedupe)")

##############################################################
#  xatlas — automatic UV unwrapping. MIT, single .h + .cpp,
#  used by Blender and Godot. Consumer: `qtmesh uv --unwrap`
#  (issue #400 under the AI-Assist epic #397).
##############################################################
FetchContent_Declare(
    xatlas
    GIT_REPOSITORY https://github.com/jpcy/xatlas.git
    GIT_TAG f700c7790aaa030e794b52ba7791a05c085faf0c  # latest master as of 2025-05; no tagged release upstream
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(xatlas)
# xatlas ships with no CMake config — wrap its `source/xatlas/xatlas.{h,cpp}` ourselves.
if(NOT TARGET xatlas)
    add_library(xatlas STATIC ${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp)
    target_include_directories(xatlas PUBLIC ${xatlas_SOURCE_DIR}/source/xatlas)
    set_target_properties(xatlas PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Build xatlas SINGLE-THREADED (XA_MULTITHREADED=0). Its internal
    # std::thread TaskScheduler livelocks on large/organic image-to-3D meshes —
    # ComputeCharts spins forever in cthread_yield/swtch_pri, freezing the app
    # with an untextured mesh (verified via process sampling, #764). Unwrap is a
    # one-off; single-threaded is slower but deadlock-free and correct.
    target_compile_definitions(xatlas PRIVATE XA_MULTITHREADED=0)
endif()
message(STATUS "xatlas enabled (auto UV unwrap)")

##############################################################
#  stb — Radiance .hdr decode for HDR environment maps (#467).
##############################################################
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    # Re-pinned 2026-08: the previous pin 31c1ad3745… stopped fetching in CI
    # ("fatal: unable to read tree" on every platform). NB stb has no upstream
    # tags/releases, so we must pin a commit SHA — and GIT_SHALLOW does NOT
    # support fetching an arbitrary SHA (only branches/tags), so it's OFF here: a
    # full clone can always resolve the pinned commit even after master advances.
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(stb)

##############################################################
#  tinyexr — OpenEXR decode for HDR environment maps (#467).
##############################################################
option(ENABLE_OPENEXR "Enable OpenEXR (.exr) HDR environment loading via tinyexr" ON)
if(ENABLE_OPENEXR)
    FetchContent_Declare(
        tinyexr
        GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
        GIT_TAG v1.0.12
        GIT_SHALLOW TRUE
    )
    # Header-only use — do NOT MakeAvailable: upstream CMake builds test
    # targets that fail macOS cross-compile (-Wpoison-system-directories).
    FetchContent_GetProperties(tinyexr)
    if(NOT tinyexr_POPULATED)
        FetchContent_Populate(tinyexr)
    endif()
    if(NOT TARGET tinyexr_miniz)
        add_library(tinyexr_miniz STATIC ${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c)
        target_include_directories(tinyexr_miniz PUBLIC
            ${tinyexr_SOURCE_DIR}/deps/miniz
            ${tinyexr_SOURCE_DIR})
        set_target_properties(tinyexr_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    add_definitions(-DENABLE_OPENEXR)
    message(STATUS "OpenEXR HDR loading enabled (tinyexr)")
else()
    message(STATUS "OpenEXR HDR loading disabled")
endif()

##############################################################
#  libsodium — minisign signature verify (auto-updater spike #440)
##############################################################
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Libsodium.cmake)

##############################################################
#  Sentry crash reporting and analytics
##############################################################
option(ENABLE_SENTRY "Enable Sentry crash reporting and analytics" ON)

if(ENABLE_SENTRY)
    include(FetchContent)
    set(SENTRY_BACKEND "inproc" CACHE STRING "" FORCE)
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SENTRY_INTEGRATION_QT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sentry
        GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
        GIT_TAG 0.7.17
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE FALSE)
    FetchContent_MakeAvailable(sentry)
    include_directories(${sentry_SOURCE_DIR}/include)
    add_definitions(-DENABLE_SENTRY)
endif()
##############################################################
#  Find Ogre
##############################################################
find_package(OGRE REQUIRED)

# On Linux, use the OGRE-detected plugin directory instead of a hardcoded path
if(UNIX AND NOT APPLE AND NOT PLUGIN_DIR)
    if(OGRE_PLUGIN_DIR)
        set(PLUGIN_DIR "${OGRE_PLUGIN_DIR}")
    else()
        set(PLUGIN_DIR "/usr/local/lib/OGRE/")
    endif()
endif()

message("OGRE_INCLUDE_DIRS: ")
message(${OGRE_INCLUDE_DIRS})
include_directories(${OGRE_INCLUDE_DIRS} "${OGRE_INCLUDE_DIRS}/Plugins/Assimp")

##############################################################
#  Find ZLIB
##############################################################
OPTION(FIX_NEED_ZLIB "Switch on if your configuration requires zlib" OFF)

if(FIX_NEED_ZLIB)
        find_package(ZLIB REQUIRED)
        set(OGRE_LIBRARIES ${OGRE_LIBRARIES} ${ZLIB_LIBRARIES})
endif()

##############################################################
#  Find Assimp
##############################################################
find_package(assimp REQUIRED)
if (assimp_FOUND)
    message("Assimp Found")
    set(ASSIMP_LIBRARY "assimp")
    add_library(${ASSIMP_LIBRARY} SHARED IMPORTED)
    set_target_properties(${ASSIMP_LIBRARY} PROPERTIES IMPORTED_LOCATION "${ASSIMP_LIBRARY_DIRS}/libassimp.so")
    include_directories(${ASSIMP_INCLUDE_DIRS})
endif(assimp_FOUND)
##############################################################
#  Adding ReadMe file
##############################################################
add_custom_target(readMeFile SOURCES README.md)
##############################################################
#  Processing Subdirs
##############################################################
ADD_SUBDIRECTORY(ui_files)
ADD_SUBDIRECTORY(src)
ADD_SUBDIRECTORY(media)
ADD_SUBDIRECTORY(profiles)
ADD_SUBDIRECTORY(cfg)

if(BUILD_TESTS)
    ADD_SUBDIRECTORY(tests)
endif()

##############################################################
#  Install Qt dependencies
##############################################################
IF(WIN32)
        MESSAGE(STATUS "Installing Qt DLLs for Windows...")
        MESSAGE(STATUS "QT_DIR: ${QT_DIR}")
        FOREACH(qtlib ${QTLIBLIST})
                MESSAGE(STATUS "Installing ${qtlib}.dll from ${QT_DIR}/../../../bin/")
                INSTALL(FILES ${QT_DIR}/../../../bin/${qtlib}.dll DESTINATION ${CMAKE_INSTALL_PREFIX})
        ENDFOREACH(qtlib)

        SET (QTPLATFORMSLIST
                qminimal
                qoffscreen
                qwindows)
        FOREACH(platform ${QTPLATFORMSLIST})
                MESSAGE(STATUS "Installing platform plugin ${platform}.dll")
                INSTALL(FILES ${QT_DIR}/../../../plugins/platforms/${platform}.dll DESTINATION ${CMAKE_INSTALL_PREFIX}/platforms/)
        ENDFOREACH(platform)
        # Qt 6: HTTPS requires TLS backend plugins (otherwise QNetworkAccessManager: "TLS initialization failed")
        if(IS_DIRECTORY "${QT_DIR}/../../../plugins/tls")
                install(DIRECTORY "${QT_DIR}/../../../plugins/tls/" DESTINATION "${CMAKE_INSTALL_PREFIX}/tls")
        endif()

ELSEIF(APPLE)
#        FOREACH(qtlib ${QTLIBLIST})
#                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.${QT_VERSION_MAJOR}.dylib DESTINATION ${CMAKE_INSTALL_PREFIX})
#        ENDFOREACH(qtlib)
ELSEIF(UNIX)
        SET (QTLIBLIST ${QTLIBLIST}
        Qt${QT_VERSION_MAJOR}XcbQpa
        Qt${QT_VERSION_MAJOR}EglFSDeviceIntegration
        Qt${QT_VERSION_MAJOR}DBus
        Qt${QT_VERSION_MAJOR}WaylandClient
        Qt${QT_VERSION_MAJOR}OpenGL)
        FOREACH(qtlib ${QTLIBLIST})
                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so.${QT_VERSION} DESTINATION ${CMAKE_INSTALL_PREFIX}/)
                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so.${QT_VERSION_MAJOR} DESTINATION ${CMAKE_INSTALL_PREFIX}/)
                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so DESTINATION ${CMAKE_INSTALL_PREFIX}/)
                
#                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so.${QT_VERSION} DESTINATION /usr/local/lib)
#                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so.${QT_VERSION_MAJOR} DESTINATION /usr/local/lib)
#                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so DESTINATION /usr/local/lib)
        ENDFOREACH(qtlib)

        SET (QTPLATFORMSLIST
                qminimal
                qxcb
                qeglfs
                qwayland-generic)
        FOREACH(platform ${QTPLATFORMSLIST})
                INSTALL(FILES ${QT_DIR}/../../../plugins/platforms/lib${platform}.so DESTINATION ${CMAKE_INSTALL_PREFIX}/platforms/)
        ENDFOREACH(platform)
        # Qt 6: HTTPS requires TLS backend plugins (otherwise QNetworkAccessManager: "TLS initialization failed")
        if(IS_DIRECTORY "${QT_DIR}/../../../plugins/tls")
                install(DIRECTORY "${QT_DIR}/../../../plugins/tls/" DESTINATION "${CMAKE_INSTALL_PREFIX}/tls")
        endif()

        # Install Qt QML modules required for Material Editor
        # When Qt libraries are bundled, Qt can't find system QML modules automatically
        get_filename_component(QT_BASE_DIR "${QT_DIR}/../../.." ABSOLUTE)
        set(QT_QML_DIR "")
        foreach(_qml_candidate
            "${QT_BASE_DIR}/qml"
            "${QT_BASE_DIR}/lib/qt6/qml"
            "${QT_BASE_DIR}/lib/x86_64-linux-gnu/qt6/qml"
            "${QT_BASE_DIR}/share/qt6/qml")
            if(EXISTS "${_qml_candidate}/QtQuick")
                set(QT_QML_DIR "${_qml_candidate}")
                break()
            endif()
        endforeach()

        if(QT_QML_DIR)
            message(STATUS "Found Qt QML modules at: ${QT_QML_DIR}")
            set(QML_MODULES_TO_INSTALL
                QtQuick
                QtQuick/Controls
                QtQuick/Controls/Basic
                QtQuick/Layouts
                QtQuick/Templates
                QtQuick/Dialogs
                QtQml
                QtQml/Models
                QtQml/WorkerScript)
            foreach(module ${QML_MODULES_TO_INSTALL})
                if(EXISTS "${QT_QML_DIR}/${module}")
                    # Get the parent directory of the module for correct installation structure
                    get_filename_component(_module_parent "${module}" DIRECTORY)
                    install(DIRECTORY "${QT_QML_DIR}/${module}"
                        DESTINATION "${CMAKE_INSTALL_PREFIX}/qml/${_module_parent}"
                        COMPONENT Runtime)
                endif()
            endforeach()
        else()
            message(WARNING "Could not find Qt QML modules directory - QML features may not work in installed version")
        endif()
ENDIF()

# This is a test for deploying using qt script, still needs more investigation.
#if(NOT APPLE)
#    # Generate the deployment script for the target QtMeshEditor.
#    qt_generate_deploy_app_script(
#        TARGET QtMeshEditor
#        FILENAME_VARIABLE deploy_script
#        NO_UNSUPPORTED_PLATFORM_ERROR
#    )

#    # Call the deployment script on "cmake --install".
#    install(SCRIPT ${deploy_script})
#endif()

##############################################################
#  DEB package
##############################################################
if(LINUX)
configure_file (
  	"${CMAKE_CURRENT_SOURCE_DIR}/DEBIAN-control.in"
    "${CMAKE_INSTALL_PREFIX}/DEBIAN-control"
        @ONLY)
endif()

##############################################################
#  CPACK
##############################################################

# Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
install(TARGETS ${APP_NAME}
    BUNDLE DESTINATION . COMPONENT Runtime
    RUNTIME DESTINATION bin COMPONENT Runtime
    )

set(CPACK_PACKAGE_NAME "QtMeshEditor")
set(CPACK_PACKAGE_VENDOR "Fernando")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A graphical editor for Ogre3D mesh, material and skeleton")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/License")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(APPLE)
    # Note Mac specific extension .app
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}.app")

    # Directories to look for dependencies
    set(DIRS ${CMAKE_INSTALL_PREFIX})
    
    set(CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    set(CPACK_GENERATOR "DEB")
    # For now, only linux is using CPACK
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Fernando Tonon <tonon.fernando@hotmail.com>")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
    set(CPACK_VERBATIM_VARIABLES ON)
#    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/QtMeshEditor")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS libicui18n.so.56 libicuuc.so.56 libicudata.so.56)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
elseif(WIN32)
    # Define NSIS installation types
    set(CPACK_ALL_INSTALL_TYPES Full Developer)
endif()

# Must be after the last CPACK macros
include(CPack)



