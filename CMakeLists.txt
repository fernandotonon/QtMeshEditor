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

project(QtMeshEditor VERSION 2.6.2 LANGUAGES CXX)
message(STATUS "Building QtMeshEditor version ${PROJECT_VERSION}")

set(QTMESHEDITOR_VERSION_STRING "\"${PROJECT_VERSION}\"")
if(WIN32 OR APPLE)
set(PLUGIN_DIR "../")
endif()

add_definitions( -DQTMESHEDITOR_VERSION=${QTMESHEDITOR_VERSION_STRING} )

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
    ADD_DEFINITIONS("-DDEBUG")
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
        ${CMAKE_CURRENT_SOURCE_DIR}/ui_files
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
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(llama_cpp)

    # Include llama.cpp headers
    include_directories(
        ${llama_cpp_SOURCE_DIR}/include
        ${llama_cpp_SOURCE_DIR}/ggml/include
    )

    add_definitions(-DENABLE_LOCAL_LLM)
    message(STATUS "Local LLM support enabled with llama.cpp")
endif()
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
        GIT_SHALLOW TRUE)
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




