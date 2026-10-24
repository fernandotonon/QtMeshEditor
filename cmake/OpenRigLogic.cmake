# OpenRigLogic (MetaHuman DNA reader + RigLogic evaluator) — epic #1095.
#
# Epic Games' OpenRigLogic is MIT-licensed (code only: sample characters ship
# separately under the Fab Standard License and are NEVER vendored here). One
# static library, `riglogic4`, contains BOTH the DNA reader/writer and the
# RigLogic runtime, so Phase 1 (DNA import) and Phase 2 (body correctives) share
# this single dependency.
#
# Pinned to the `5.8` stable branch (the upstream "production-ready" line,
# immutable except for hotfixes) rather than `main`, which is explicitly
# experimental. Bump QTMESH_OPENRIGLOGIC_TAG deliberately and re-run the DNA
# tests: the binary DNA format is versioned and a bump can change what loads.
#
# Why FetchContent (not find_package): OpenRigLogic is not packaged by brew /
# apt / MinGW, and one pinned source tree gives the same behaviour on all three
# platforms — the cmake/Alembic.cmake rationale.
#
# Exposes the imported target `qtmesh_openriglogic`; the C++ side is
# `#ifdef ENABLE_OPENRIGLOGIC`-guarded so a build without it still compiles.

if(TARGET qtmesh_openriglogic)
    return()
endif()

include(FetchContent)

# RigLogic 13.2.9 (UE 5.8.3 snapshot, on the 5.8 stable branch): the version the Phase 1 spike was validated against.
# History note: upstream rewrote the repository's history on 2026-10-22/23 (every commit is now an "Import RigLogic vX snapshot"),
# which deleted the commit this used to pin (55756aca...) and broke every CI job at the FetchContent clone. The pin is now one of the
# new snapshot commits, which are published on 5.8 (and main) and are not rewritten per release. If it ever disappears again, pick
# the 13.2.x snapshot on the 5.8 branch and re-run the DNA tests.
set(QTMESH_OPENRIGLOGIC_TAG "1b20901ec6fcef119eed71a23a60ae10b3c83a44"
    CACHE STRING "OpenRigLogic git revision (5.8 stable branch, RigLogic 13.2.9)")

# Upstream regenerates Version.h / Defs.h INSIDE its source tree unless told not
# to — a FetchContent checkout is shared by every build dir, so keep it
# read-only (the generated headers are committed upstream).
set(READ_ONLY_SOURCE_TREE ON  CACHE BOOL "" FORCE)
set(RL_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(RL_BUILD_BENCHMARKS   OFF CACHE BOOL "" FORCE)
set(RL_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(RL_BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
set(RL_BUILD_FUZZER       OFF CACHE BOOL "" FORCE)
set(RL_BUILD_PYTHON_WRAPPER "" CACHE STRING "" FORCE)
# Static, regardless of how the parent project sets BUILD_SHARED_LIBS.
set(RL_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)

FetchContent_Declare(
    qtmesh_openriglogic_src
    GIT_REPOSITORY https://github.com/EpicGames/OpenRigLogic.git
    GIT_TAG        ${QTMESH_OPENRIGLOGIC_TAG}
)
FetchContent_MakeAvailable(qtmesh_openriglogic_src)

# MIT requires the notice to accompany copies: ship it with every package the
# way the QuadriFlow helper's licences travel (cmake/QuadriFlow.cmake).
install(FILES "${qtmesh_openriglogic_src_SOURCE_DIR}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_PREFIX}
        RENAME OpenRigLogic.LICENSE)

# JSON DNA (the library's text format; JSONStreamReader/Writer) is compiled only under this define,
# which must reach the library's own sources AND every includer of its headers (the classes are
# declared inside the same #ifdef), hence PUBLIC. It is self-contained (terse's JSON archive, no
# external dependency).
if(TARGET riglogic4)
    target_compile_definitions(riglogic4 PUBLIC DNA_BUILD_WITH_JSON_SUPPORT)
endif()

# Upstream forces C++11 only when CMAKE_CXX_STANDARD is unset, so it follows ours.
add_library(qtmesh_openriglogic INTERFACE)
target_link_libraries(qtmesh_openriglogic INTERFACE RigLogic::riglogic4)
