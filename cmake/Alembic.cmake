# Alembic (.abc) vertex-animation import — Anim epic Slice B (#519), sub-slice B2.
#
# Vendors Imath 3 + Alembic (both BSD-3-Clause) via FetchContent, statically,
# with every optional component OFF (no HDF5, Python, tests, binaries, install).
# Exposes an imported target `qtmesh_alembic` that the app links; the C++ side
# is #ifdef ENABLE_ALEMBIC-guarded so a build without this still compiles.
#
# Why build from source rather than find_package: Alembic + Imath system
# packages are absent or version-skewed across our three targets (macOS via
# brew, Ubuntu CI, Windows MinGW). FetchContent gives one reproducible version
# everywhere, matching how the project already vendors ONNX Runtime / libsodium
# / tinyexr.
#
# Dependency chain: Alembic FIND_PACKAGE(Imath) → we build Imath as a
# subproject first, point Imath_DIR at its generated build-tree config so
# Alembic's find_package(Imath CONFIG) resolves to the target we just built.

if(TARGET qtmesh_alembic)
    return()
endif()

include(FetchContent)

set(QTMESH_IMATH_TAG   "v3.1.12"  CACHE STRING "Imath git tag")
set(QTMESH_ALEMBIC_TAG "1.8.8"    CACHE STRING "Alembic git tag")

# ---- Imath ---------------------------------------------------------------
# Static, no tests/python. IMATH_INSTALL stays ON: Alembic's own
# INSTALL(EXPORT AlembicTargets) is unconditional and references Imath, so
# Imath MUST be in an export set too or CMake's generate step fails
# ("target Alembic requires target Imath that is not in any export set").
# We never actually run `make install` from the app build, so an ON install
# rule is harmless — it just keeps the export sets consistent.
set(IMATH_INSTALL           ON  CACHE BOOL "" FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(PYTHON                  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    qtmesh_imath
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/Imath.git
    GIT_TAG        ${QTMESH_IMATH_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(qtmesh_imath)

# Alembic's find_package(Imath) resolves against a CONFIG. Imath-as-subproject
# writes its package config under the build dir; point find_package there.
if(NOT DEFINED Imath_DIR)
    set(Imath_DIR "${qtmesh_imath_BINARY_DIR}/config" CACHE PATH "" FORCE)
endif()

# ---- Alembic -------------------------------------------------------------
# Everything optional OFF: no HDF5 backend (we only read the modern Ogawa
# backend), no tests/binaries/python/prman/maya/arnold, static lib.
set(USE_HDF5            OFF CACHE BOOL "" FORCE)
set(USE_TESTS          OFF CACHE BOOL "" FORCE)
set(USE_BINARIES       OFF CACHE BOOL "" FORCE)
set(USE_EXAMPLES       OFF CACHE BOOL "" FORCE)
set(USE_PYALEMBIC      OFF CACHE BOOL "" FORCE)
set(USE_ARNOLD         OFF CACHE BOOL "" FORCE)
set(USE_PRMAN          OFF CACHE BOOL "" FORCE)
set(USE_MAYA           OFF CACHE BOOL "" FORCE)
set(ALEMBIC_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ALEMBIC_ILMBASE_LINK_STATIC ON CACHE BOOL "" FORCE)
set(ALEMBIC_LIB_INSTALL_DIR "lib" CACHE STRING "" FORCE)

FetchContent_Declare(
    qtmesh_alembic
    GIT_REPOSITORY https://github.com/alembic/alembic.git
    GIT_TAG        ${QTMESH_ALEMBIC_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(qtmesh_alembic)

# Alembic's core library target is `Alembic` (with an `Alembic::Alembic` alias
# in recent versions). Wrap whichever exists behind our stable name so the app
# links `qtmesh_alembic` regardless.
if(TARGET Alembic::Alembic)
    add_library(qtmesh_alembic INTERFACE)
    target_link_libraries(qtmesh_alembic INTERFACE Alembic::Alembic Imath::Imath)
elseif(TARGET Alembic)
    add_library(qtmesh_alembic INTERFACE)
    target_link_libraries(qtmesh_alembic INTERFACE Alembic Imath::Imath)
    # The plain `Alembic` target's public include dirs cover its own headers;
    # Imath::Imath carries the Imath headers Alembic's public API exposes.
else()
    message(FATAL_ERROR "Alembic.cmake: neither Alembic nor Alembic::Alembic target was created")
endif()

message(STATUS "Alembic ${QTMESH_ALEMBIC_TAG} + Imath ${QTMESH_IMATH_TAG} vendored (static)")
