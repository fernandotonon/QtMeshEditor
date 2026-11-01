# Applies each patch in PATCHES (a ;-list) to the git checkout in the working
# directory, skipping any that is already applied. FetchContent re-runs its
# PATCH_COMMAND on every reconfigure, and a plain `git apply` fails the second
# time on an already-patched tree.
foreach(patch IN LISTS PATCHES)
    execute_process(COMMAND git apply --check --reverse --ignore-whitespace "${patch}"
                    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(already EQUAL 0)
        message(STATUS "patch already applied: ${patch}")
        continue()
    endif()
    execute_process(COMMAND git apply --ignore-whitespace "${patch}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "failed to apply ${patch}: ${err}")
    endif()
endforeach()
