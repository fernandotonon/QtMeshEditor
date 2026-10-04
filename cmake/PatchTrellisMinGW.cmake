# Idempotent source patch for the bundled trellis.cpp checkout (run as the
# ExternalProject PATCH_COMMAND, all platforms — it no-ops where the pattern
# is already guarded).
#
# Qt's MinGW 13.1 toolchain ships mingw-w64 headers that predate the Windows 11
# thread-power-throttling API: THREAD_POWER_THROTTLING_STATE and friends are
# missing (only the PROCESS_* variants exist), so trellis.cpp's vendored
# ggml-cpu.c fails to compile even though the block is _WIN32_WINNT-guarded.
# The block is a perf-only opt-out (keeps Win11 from parking cores) — extend
# its guard so header sets without the API simply skip it, exactly what the
# code already does for pre-8 Windows targets.
#
# Usage: cmake -DSRC=<trellis src dir> -P PatchTrellisMinGW.cmake
if(NOT SRC)
    message(FATAL_ERROR "PatchTrellisMinGW.cmake: pass -DSRC=<source dir>")
endif()

set(_file "${SRC}/thirdparty/ggml/src/ggml-cpu/ggml-cpu.c")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "PatchTrellisMinGW.cmake: ${_file} not found — did the pin move?")
endif()

file(READ "${_file}" _content)
set(_old "#if _WIN32_WINNT >= 0x0602\n        THREAD_POWER_THROTTLING_STATE t;")
set(_new "#if _WIN32_WINNT >= 0x0602 && defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)\n        THREAD_POWER_THROTTLING_STATE t;")

string(FIND "${_content}" "${_new}" _already)
if(NOT _already EQUAL -1)
    message(STATUS "trellis.cpp MinGW throttling guard already applied")
    return()
endif()

string(FIND "${_content}" "${_old}" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "PatchTrellisMinGW.cmake: expected pattern not found in "
                        "ggml-cpu.c — the pin moved; re-check whether upstream "
                        "fixed the MinGW guard and drop this patch if so.")
endif()

string(REPLACE "${_old}" "${_new}" _content "${_content}")
file(WRITE "${_file}" "${_content}")
message(STATUS "trellis.cpp: applied MinGW thread-power-throttling guard")
