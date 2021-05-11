# /*
# -----------------------------------------------------------------------------
# This source file is part of
#                                     _
#   ___   __ _ _ __ ___  __ _ ___ ___(_)_ __ ___  _ __
#  / _ \ / _` | '__/ _ \/ _` / __/ __| | '_ ` _ \| '_ \
# | (_) | (_| | | |  __/ (_| \__ \__ \ | | | | | | |_) |
#  \___/ \__, |_|  \___|\__,_|___/___/_|_| |_| |_| .__/
#        |___/                                   |_|
#
# For the latest info, see http://code.google.com/p/ogreassimp/
#
# Copyright (c) 2011 Jacob 'jacmoe' Moen
#
# Licensed under the MIT license:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
# -----------------------------------------------------------------------------
#*/
cmake_minimum_required(VERSION 2.8)

set(library_name OgreAssimpLoader)

set(OGREASSIMP_VERSION_STRING "0.1.1")
set(OGREASSIMP_VERSION_STRING_MAJOR "0")
set(OGREASSIMP_VERSION_STRING_MINOR "1")
set(OGREASSIMP_VERSION_STRING_PATCH "1")


if("${OGRE_VERSION_NAME}" STREQUAL "Byatis")
    add_definitions(-DUSING_BYATIS)
endif()

set(HDRS
    ./AssimpLoader.h
)

set(SRCS
    ./AssimpLoader.cpp
)

include_directories( ${OIS_INCLUDE_DIRS}
    ${OGRE_INCLUDE_DIRS}
    ${ASSIMP_INCLUDE_DIRS}
)

add_library(${library_name} STATIC ${HDRS} ${SRCS})
set_target_properties(${library_name} PROPERTIES DEBUG_POSTFIX _d)

target_link_libraries(${library_name} ${OGRE_LIBRARIES} ${ASSIMP_LIBRARIES})

# definitions for using lib
set(OGRE_ASSIMP_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
set(OGREASSIMP_LIBRARY ${library_name} PARENT_SCOPE)

install(FILES ${HDRS} DESTINATION ${BUILD_INCLUDE_DIR}/${library_name})


