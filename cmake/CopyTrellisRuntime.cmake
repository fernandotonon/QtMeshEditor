# Copy the bundled trellis-cli (+ any ggml shared libs it still needs) into DEST.
# Used from POST_BUILD / install(CODE) so packaging never ships a dangling
# dynamic link against libggml.so that isn't next to the binary ($ORIGIN) or
# on the editor's private lib path.
#
#   cmake -DDEST=<dir> -DCLI=<trellis-cli path> -DBUILD_DIR=<trellis build dir>
#         [-DLIC=<license file>] -P CopyTrellisRuntime.cmake
if(NOT DEST OR NOT CLI OR NOT BUILD_DIR)
    message(FATAL_ERROR
        "CopyTrellisRuntime.cmake: DEST, CLI and BUILD_DIR are required")
endif()
if(NOT EXISTS "${CLI}")
    message(FATAL_ERROR "CopyTrellisRuntime.cmake: CLI not found: ${CLI}")
endif()

file(MAKE_DIRECTORY "${DEST}")
file(COPY "${CLI}" DESTINATION "${DEST}" FILE_PERMISSIONS
     OWNER_READ OWNER_WRITE OWNER_EXECUTE
     GROUP_READ GROUP_EXECUTE
     WORLD_READ WORLD_EXECUTE)

if(LIC AND EXISTS "${LIC}")
    file(COPY "${LIC}" DESTINATION "${DEST}")
endif()

# Defense in depth: even with BUILD_SHARED_LIBS=OFF some backends may still
# emit shared libs; ship whatever landed next to the CLI in the ExternalProject
# build so $ORIGIN / the private lib dir can resolve them.
file(GLOB _ggml_libs
    "${BUILD_DIR}/libggml*"
    "${BUILD_DIR}/ggml*.dll"
    "${BUILD_DIR}/libggml*.dylib"
    "${BUILD_DIR}/ggml*.dylib")
foreach(_lib IN LISTS _ggml_libs)
    if(IS_DIRECTORY "${_lib}")
        continue()
    endif()
    # Skip cmake intermediate files / import libs that aren't loadable.
    get_filename_component(_ext "${_lib}" EXT)
    if(_ext STREQUAL ".a" OR _ext STREQUAL ".lib" OR _ext STREQUAL ".prl")
        continue()
    endif()
    file(COPY "${_lib}" DESTINATION "${DEST}")
endforeach()
