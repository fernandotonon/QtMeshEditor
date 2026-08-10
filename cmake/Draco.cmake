# Draco.cmake — locate the Draco mesh-compression library for MeshDracoEncoder
# (glTF KHR_draco_mesh_compression export, issue #506).
#
# Draco ships vendored inside Assimp (contrib/draco). Building Assimp with
# -DASSIMP_BUILD_DRACO=ON compiles that vendored copy and installs libdraco
# (static) + the draco/ headers under the Assimp install prefix. This module
# discovers those, or a standalone Draco build pointed at by -DDRACO_ROOT.
#
# Resolution order:
#   1. DRACO_ROOT (cache/CLI var)         -> <DRACO_ROOT>/{lib,include}
#   2. assimp_DIR-derived install prefix  -> where ASSIMP_BUILD_DRACO installs it
#   3. CMAKE_PREFIX_PATH / system paths    -> find_library/find_path defaults
#
# On success defines the imported target `qtmesh_draco` (INTERFACE) carrying
# the include dir + the static lib, and sets QTMESH_DRACO_FOUND.

set(QTMESH_DRACO_FOUND FALSE)

# Candidate roots to search.
set(_draco_hint_roots "")
if(DEFINED DRACO_ROOT)
    list(APPEND _draco_hint_roots "${DRACO_ROOT}")
endif()
if(DEFINED assimp_DIR)
    # assimp_DIR is <prefix>/lib/cmake/assimp-X.Y — walk up to <prefix>.
    get_filename_component(_assimp_prefix "${assimp_DIR}/../../.." ABSOLUTE)
    list(APPEND _draco_hint_roots "${_assimp_prefix}")
endif()
if(DEFINED ASSIMP_DIR)
    get_filename_component(_assimp_prefix2 "${ASSIMP_DIR}/../../.." ABSOLUTE)
    list(APPEND _draco_hint_roots "${_assimp_prefix2}")
endif()

find_path(DRACO_INCLUDE_DIR
    NAMES draco/compression/encode.h
    HINTS ${_draco_hint_roots}
    PATH_SUFFIXES include
)

# Draco installs as libdraco.a (static) or the per-module libs; the umbrella
# static archive is what a standalone or Assimp-vendored build produces.
find_library(DRACO_LIBRARY
    NAMES draco libdraco draco_static
    HINTS ${_draco_hint_roots}
    PATH_SUFFIXES lib lib64
)

if(DRACO_INCLUDE_DIR AND DRACO_LIBRARY)
    add_library(qtmesh_draco INTERFACE)
    target_include_directories(qtmesh_draco INTERFACE "${DRACO_INCLUDE_DIR}")
    target_link_libraries(qtmesh_draco INTERFACE "${DRACO_LIBRARY}")
    set(QTMESH_DRACO_FOUND TRUE)
    message(STATUS "Draco found: ${DRACO_LIBRARY}")
    message(STATUS "Draco headers: ${DRACO_INCLUDE_DIR}")
else()
    message(WARNING
        "ENABLE_DRACO is ON but the Draco library was not found.\n"
        "  Searched roots: ${_draco_hint_roots}\n"
        "  DRACO_INCLUDE_DIR=${DRACO_INCLUDE_DIR}\n"
        "  DRACO_LIBRARY=${DRACO_LIBRARY}\n"
        "Build Assimp with -DASSIMP_BUILD_DRACO=ON, or pass -DDRACO_ROOT=<dir> "
        "pointing at a Draco install (with lib/ and include/draco).")
endif()
