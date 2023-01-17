#############################################################
#
# On Windows, define the following environnemental variables before running:
#
# - OGRE_HOME (C:\OgreSDK)
# - ASSIMP_HOME (C:\Program Files\Assimp)
# - CMAKE_PREFIX_PATH (to Qt <version>\<compiler>\lib\cmake folder)
# - OgreProcedural_HOME (Ogre Procedural SDK Folder)
#
# TODO: Add ogre-procedural as dependency
##############################################################
#  Versions
##############################################################
CMAKE_POLICY(SET CMP0005 NEW)

project(QtMeshEditor)

set(QTMESHEDITOR_VERSION_STRING "\"1.5.1\"")

add_definitions( -DQTMESHEDITOR_VERSION=${QTMESHEDITOR_VERSION_STRING} )
message(STATUS "Building QtMeshEditor version ${QTMESHEDITOR_VERSION_STRING}")

##############################################################
#  Configuring CMake
##############################################################
if(WIN32)
    cmake_policy(SET CMP0020 NEW)   # to avoid cmake warning
endif()
cmake_minimum_required(VERSION 3.20.0)

enable_language(CXX)

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

#set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/;${CMAKE_MODULE_PATH}")

##############################################################
#  Find Ogre
##############################################################
# We provide a FindOgre.cmake as the one provided by Ogre has a bug on linux (version number in the default install path)

find_package(OGRE REQUIRED)

include_directories(${OGRE_INCLUDE_DIRS} "${OGRE_INCLUDE_DIR}/Plugins/Assimp")

##############################################################
#  Find ASSIMP
##############################################################
find_package(ASSIMP REQUIRED)
include_directories(${ASSIMP_INCLUDE_DIRS})

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
#IF(WIN32)
#        SET (QTLIBLIST
#        Qt${QT_VERSION_MAJOR}GUI
#        Qt${QT_VERSION_MAJOR}Core
#        Qt${QT_VERSION_MAJOR}Widgets)
#        FOREACH(qtlib ${QTLIBLIST})
#                INSTALL(FILES ${QT_DIR}/../../../bin/${qtlib}.dll DESTINATION ${CMAKE_INSTALL_PREFIX})
#        ENDFOREACH(qtlib)

#        SET (QTPLATFORMSLIST
#                qminimal
#                qoffscreen
#                qwindows)
#        FOREACH(platform ${QTPLATFORMSLIST})
#                INSTALL(FILES ${QT_DIR}/../../../plugins/platforms/${platform}.dll DESTINATION ${CMAKE_INSTALL_PREFIX}/platforms/)
#        ENDFOREACH(platform)
#ELSEIF(UNIX)
#        SET (QTLIBLIST
#        Qt${QT_VERSION_MAJOR}Gui
#        Qt${QT_VERSION_MAJOR}Core
#        Qt${QT_VERSION_MAJOR}Widgets
#        Qt${QT_VERSION_MAJOR}XcbQpa)
#        FOREACH(qtlib ${QTLIBLIST})
#                INSTALL(FILES ${QT_DIR}/../../lib${qtlib}.so.${QT_VERSION_MAJOR} DESTINATION ${CMAKE_INSTALL_PREFIX})
#        ENDFOREACH(qtlib)

#        SET (QTPLATFORMSLIST
#                qminimal
#                qxcb
#                qeglfs
#                qwayland-generic)
#        FOREACH(platform ${QTPLATFORMSLIST})
#                INSTALL(FILES ${QT_DIR}/../../../plugins/platforms/lib${platform}.so DESTINATION ${CMAKE_INSTALL_PREFIX}/platforms/)
#        ENDFOREACH(platform)
#ENDIF()

if(NOT APPLE)
    # Install the executable into "${CMAKE_INSTALL_PREFIX}/bin".
    install(TARGETS QtMeshEditor
        BUNDLE  DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Generate the deployment script for the target QtMeshEditor.
    qt_generate_deploy_app_script(
        TARGET QtMeshEditor
        FILENAME_VARIABLE deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )

    # Call the deployment script on "cmake --install".
    install(SCRIPT ${deploy_script})
endif()

if(UNIX)
    set(CPACK_PACKAGE_NAME QtMeshEditor)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A graphical editor for Ogre3D mesh, material and skeleton")
    set(CPACK_PACKAGE_VENDOR "Fernando")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
    set(CPACK_VERBATIM_VARIABLES ON)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/QtMeshEditor")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Fernando Tonon <tonon.fernando@hotmail.com>")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS libc6 libstdc++6 libgcc-s1)
    include(CPack)
endif()
