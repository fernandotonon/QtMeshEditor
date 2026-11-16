# libwebp (BSD-3-Clause) — WebP DECODER only, for textures embedded as WebP.
#
# Why: glTF files exported by trimesh (TRELLIS.2's own exports, many web
# pipelines) embed textures as WebP through EXT_texture_webp. Assimp >= 6.0.3
# resolves the extension's image, but Ogre's stb_image codec cannot decode
# WebP and our Qt build has no WebP image plugin, so the texture was dropped
# silently and the model rendered untextured. MaterialProcessor decodes WebP
# itself through `qtmesh_webp` (WebPImage.cpp).
#
# Only the `webpdecoder` target is linked (decode + its dsp/utils); every tool
# and the mux/extras libraries are switched off. Always built: it is small and
# a file format we can open should not depend on a build flag.

if(TARGET qtmesh_webp)
    return()
endif()

include(FetchContent)

set(QTMESH_LIBWEBP_TAG "v1.5.0" CACHE STRING "libwebp git tag")

FetchContent_Declare(
    qtmesh_libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        ${QTMESH_LIBWEBP_TAG}
    GIT_SHALLOW    TRUE
)

# block() keeps these settings (notably BUILD_SHARED_LIBS) from leaking into
# the rest of the project.
block()
    set(BUILD_SHARED_LIBS        OFF)
    set(WEBP_BUILD_ANIM_UTILS    OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP         OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP         OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP         OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPMUX    OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX       OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS        OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBP_JS       OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_FUZZTEST      OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(qtmesh_libwebp)
endblock()
# MakeAvailable's *_SOURCE_DIR was set inside the block scope; read it back.
FetchContent_GetProperties(qtmesh_libwebp)

add_library(qtmesh_webp INTERFACE)
target_link_libraries(qtmesh_webp INTERFACE webpdecoder)
# Upstream's interface include is the repository root ("src/webp/decode.h");
# expose src/ so callers write <webp/decode.h>.
target_include_directories(qtmesh_webp INTERFACE ${qtmesh_libwebp_SOURCE_DIR}/src)
set_target_properties(webpdecoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
message(STATUS "libwebp ${QTMESH_LIBWEBP_TAG} enabled (WebP texture decoding)")
