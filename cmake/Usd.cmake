# USD (.usd/.usda/.usdc/.usdz) import + export — Anim epic Slice J (#527).
#
# Vendors LightUSD (formerly TinyUSDZ; Apache-2.0, dependency-free C++17, no
# Boost/TBB/Python) via FetchContent, statically, with everything optional off.
# Exposes the imported target `qtmesh_usd` that the app links; the C++ side is
# #ifdef ENABLE_USD-guarded so a build without this still compiles and links.
#
# Why LightUSD and not Pixar's OpenUSD: OpenUSD needs Boost-era dependencies
# (TBB, OpenSubdiv optional, a plugin registry) and has no maintained MinGW
# story; LightUSD builds as one static library through add_subdirectory on all
# three of our targets and reads AND writes USDA/USDC/USDZ. It is a smaller,
# newer implementation — complex composition / Hydra-only schemas are less
# complete — but the surface this editor needs (meshes, materials, skeletons,
# SkelAnimation, blend shapes, xform animation) is covered.
#
# Pin: a commit, not a branch — upstream moves quickly. Bump it deliberately and
# re-run the UsdBridge tests. For an offline/local build point CMake at an
# existing checkout with -DFETCHCONTENT_SOURCE_DIR_QTMESH_LIGHTUSD=<dir>.

if(TARGET qtmesh_usd)
    return()
endif()

include(FetchContent)

set(QTMESH_LIGHTUSD_TAG "1fdd60a6171724b5ce0e6716c25b2bba1aed7ad3"
    CACHE STRING "LightUSD git commit")

# Everything optional OFF: the app only needs the core reader/writer + Tydra
# (Stage -> RenderScene). No MaterialX, audio, FBX/OBJ loaders, JSON, C API,
# tools, tests or examples.
set(LIGHTUSD_WITH_USDMTLX  OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_WITH_AUDIO    OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_WITH_USDFBX   OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_WITH_USDOBJ   OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_WITH_JSON     OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_WITH_C_API    OFF CACHE BOOL "" FORCE)
set(LIGHTUSD_USE_CCACHE    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    qtmesh_lightusd
    GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
    GIT_TAG        ${QTMESH_LIGHTUSD_TAG}
    # A commit hash cannot be fetched shallowly on every git host.
    GIT_SHALLOW    FALSE
    # LightUSD's crate (.usdc/.usdz) writer compresses `token[]` arrays of 16+
    # elements; OpenUSD readers (Blender, usdview) expect them raw and read
    # garbage — a rig with 16+ joints lost its whole skeleton. Applied on
    # a fresh checkout, skipped when already applied (a FETCHCONTENT_SOURCE_DIR checkout is not patched:
    # apply cmake/patches/*.patch to it yourself).
    # Second patch: an unknown texture colorSpace tag (Blender writes
    # "srgb_rec709_display") failed the whole import; it now warns.
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   "-DPATCHES=${CMAKE_CURRENT_LIST_DIR}/patches/lightusd-uncompressed-token-arrays.patch;${CMAKE_CURRENT_LIST_DIR}/patches/lightusd-lenient-colorspace.patch"
                   -P "${CMAKE_CURRENT_LIST_DIR}/ApplyPatchesOnce.cmake"
)
FetchContent_MakeAvailable(qtmesh_lightusd)

# LightUSD builds `lightusd_static` (alias lightusd::lightusd_static). Wrap it
# behind our stable name and add the header root, which the upstream target does
# not export (the sources include "lightusd.hh", "tydra/render-data.hh", ...).
if(TARGET lightusd::lightusd_static)
    set(_qtmesh_usd_lib lightusd::lightusd_static)
elseif(TARGET lightusd_static)
    set(_qtmesh_usd_lib lightusd_static)
else()
    message(FATAL_ERROR "LightUSD did not provide a lightusd_static target")
endif()

add_library(qtmesh_usd INTERFACE)
target_link_libraries(qtmesh_usd INTERFACE ${_qtmesh_usd_lib})
target_include_directories(qtmesh_usd SYSTEM INTERFACE
    "${qtmesh_lightusd_SOURCE_DIR}/src"
    "${qtmesh_lightusd_SOURCE_DIR}/src/external")
