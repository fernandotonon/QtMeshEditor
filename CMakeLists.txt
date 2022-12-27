#############################################################
#
# On Windows, define the following environnemental variables before running:
#
# - OGRE_HOME (C:\OgreSDK)
# - ASSIMP_HOME (C:\Program Files\Assimp)
# - BOOST_ROOT (It is recommended that you use the Boost library coming with ogre)
# - CMAKE_PREFIX_PATH (C:\Qt\Qt5.3.1\5.3\mingw482_32\lib\cmake)
# - OgreProcedural_HOME (Ogre Procedural SDK Folder)
#
# TODO: Add ogre-procedural as dependency
##############################################################
#  Versions
##############################################################
CMAKE_POLICY(SET CMP0005 NEW)

project(QtMeshEditor)

set(QTMESHEDITOR_VERSION_STRING "\"1.4.7\"")

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
find_package(Qt6 COMPONENTS Core Widgets Gui)
if (NOT Qt6_FOUND)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets Gui)
endif()

if(UNIX)
    find_package(Qt6 COMPONENTS X11Extras)
    if (NOT Qt6_FOUND)
        find_package(Qt5X11Extras REQUIRED)
    endif()
endif()


set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/;${CMAKE_MODULE_PATH}")


##############################################################
#  Find OgreProcedural
##############################################################
find_package(OgreProcedural REQUIRED)
include_directories(${OgreProcedural_INCLUDE_DIRS})

##############################################################
#  Find BOOST
##############################################################
if(NOT OGRE_BUILD_PLATFORM_IPHONE)
        if(WIN32 OR APPLE)
                set(Boost_USE_STATIC_LIBS TRUE)
        else()
                # Statically linking boost to a dynamic Ogre build doesn't work on Linux 64bit
                set(Boost_USE_STATIC_LIBS ${OGRE_STATIC})
        endif()
        if(MINGW)
                # this is probably a bug in CMake: the boost find module tries to look for
                # boost libraries with name libboost_*, but CMake already prefixes library
                # search names with "lib". This is the workaround.
                set(CMAKE_FIND_LIBRARY_PREFIXES ${CMAKE_FIND_LIBRARY_PREFIXES} "")
        endif()
        set(Boost_ADDITIONAL_VERSIONS "1.50" "1.51" "1.51.0" "1.52" "1.52.0" "1.53" "1.53.0" "1.54" "1.54.0" "1.55" "1.55.0")
        # Components that need linking (NB does not include header-only components like bind)
        set(OGRE_BOOST_COMPONENTS thread system REQUIRED)
        find_package(Boost COMPONENTS ${OGRE_BOOST_COMPONENTS} QUIET)
        if (NOT Boost_FOUND)
                # Try again with the other type of libs
                set(Boost_USE_STATIC_LIBS NOT ${Boost_USE_STATIC_LIBS})
                find_package(Boost COMPONENTS ${OGRE_BOOST_COMPONENTS} QUIET)
        endif()
        #find_package(Boost QUIET)

        # Set up referencing of Boost
        include_directories(${Boost_INCLUDE_DIR}/boost)
        add_definitions(-DBOOST_ALL_NO_LIB)
        set(BOOST_LIB ${Boost_LIBRARIES})
endif()
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
IF(WIN32)
        SET (QTLIBLIST
        Qt${QT_VERSION_MAJOR}GUI
        Qt${QT_VERSION_MAJOR}Core
        Qt${QT_VERSION_MAJOR}Widgets)
        FOREACH(qtlib ${QTLIBLIST})
                INSTALL(FILES ${QT_DIR}/../../../bin/${qtlib}.dll DESTINATION ${CMAKE_INSTALL_PREFIX})
        ENDFOREACH(qtlib)
endif()
IF(WIN32)
        SET (QTPLATFORMSLIST
                qminimal
                qoffscreen
                qwindows)
        FOREACH(platform ${QTPLATFORMSLIST})
                INSTALL(FILES ${QT_DIR}/../../../plugins/platforms/${platform}.dll DESTINATION ${CMAKE_INSTALL_PREFIX}/platforms/)
        ENDFOREACH(platform)
ENDIF(WIN32)
