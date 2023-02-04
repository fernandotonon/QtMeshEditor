#############################################################
#
# On Windows, define the following environnemental variables before running:
#
# - OGRE_HOME (C:\OgreSDK)
# - CMAKE_PREFIX_PATH (to Qt <version>\<compiler>\lib\cmake folder)
# - OgreProcedural_HOME (Ogre Procedural SDK Folder)
#
# TODO: Add ogre-procedural as dependency
##############################################################
#  Versions
##############################################################
cmake_minimum_required(VERSION 3.24.0)
cmake_policy(SET CMP0005 NEW)
cmake_policy(SET CMP0048 NEW) # manages project version

project(QtMeshEditor VERSION 1.5.2 LANGUAGES CXX)
message(STATUS "Building QtMeshEditor version ${PROJECT_VERSION}")

set(QTMESHEDITOR_VERSION_STRING "\"${PROJECT_VERSION}\"")
if(WIN32 OR APPLE)
set(PLUGIN_DIR "../")
elseif(UNIX)
set(PLUGIN_DIR "/lib/x86_64-linux-gnu/")
endif()

add_definitions( -DQTMESHEDITOR_VERSION=${QTMESHEDITOR_VERSION_STRING} )

set(CMAKE_CXX_STANDARD 17)

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
#Find Qt Packages
find_package(QT NAMES Qt6 Qt5)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)
if (NOT Qt6_FOUND)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets Gui)
endif()

if(UNIX)
    if (NOT Qt6_FOUND)
        find_package(Qt5X11Extras REQUIRED)
    endif()
endif()
message("Found Qt (${QT_VERSION_MAJOR})")

SET (QTLIBLIST
    Qt${QT_VERSION_MAJOR}Gui
    Qt${QT_VERSION_MAJOR}Core
    Qt${QT_VERSION_MAJOR}Widgets
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
      URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
##############################################################
#  Find Ogre
##############################################################
# We provide a FindOgre.cmake as the one provided by Ogre has a bug on linux (version number in the default install path)

find_package(OGRE REQUIRED)

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

##############################################################
#  Install Qt dependencies
##############################################################
IF(WIN32)
        FOREACH(qtlib ${QTLIBLIST})
                INSTALL(FILES ${QT_DIR}/../../../bin/${qtlib}.dll DESTINATION ${CMAKE_INSTALL_PREFIX})
        ENDFOREACH(qtlib)

        SET (QTPLATFORMSLIST
                qminimal
                qoffscreen
                qwindows)
        FOREACH(platform ${QTPLATFORMSLIST})
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

    # this is causing issues in macos, when trying to find assimp in @rpath/libassimp.5.dylib
#    install(CODE "include(BundleUtilities)
#        fixup_bundle(\"${APPS}\" \"\" \"${DIRS}\" IGNORE_ITEM \"@rpath/libassimp.5.dylib\")")
    set(CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
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
