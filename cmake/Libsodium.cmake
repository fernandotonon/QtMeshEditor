# libsodium for minisign verify (#440 / #445).
# Linux + macOS link static/system libsodium. Windows MinGW verify is deferred
# (autotools/cmake build not wired in CI yet — download still runs, verify fails closed).

if(TARGET qtmesh_sodium)
    return()
endif()

if(WIN32)
    message(STATUS "libsodium: Windows MinGW verify deferred (#445 follow-up)")
    add_library(qtmesh_sodium INTERFACE)
    target_compile_definitions(qtmesh_sodium INTERFACE QTMESH_MINISIGN_VERIFY=0)
    return()
endif()

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(QTMESH_LIBSODIUM libsodium)
    endif()
    if(QTMESH_LIBSODIUM_FOUND)
        message(STATUS "libsodium: using system package (pkg-config)")
        add_library(qtmesh_sodium INTERFACE)
        target_include_directories(qtmesh_sodium SYSTEM INTERFACE ${QTMESH_LIBSODIUM_INCLUDE_DIRS})
        target_link_libraries(qtmesh_sodium INTERFACE ${QTMESH_LIBSODIUM_LIBRARIES})
        target_compile_options(qtmesh_sodium INTERFACE ${QTMESH_LIBSODIUM_CFLAGS_OTHER})
        target_compile_definitions(qtmesh_sodium INTERFACE QTMESH_MINISIGN_VERIFY=1)
        return()
    endif()
endif()

include(FetchContent)

set(QTMESH_LIBSODIUM_URL
    "https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable.tar.gz"
    CACHE STRING "libsodium source tarball for minisign verify")
set(QTMESH_LIBSODIUM_SHA256
    "b6b1d2a8802cd8bfa611638c0e9ce31d14ef324e16062c39a76854091cba6d7f"
    CACHE STRING "SHA256 of libsodium tarball")

message(STATUS "libsodium: building static from source")

FetchContent_Declare(
    qtmesh_libsodium_src
    URL ${QTMESH_LIBSODIUM_URL}
    URL_HASH SHA256=${QTMESH_LIBSODIUM_SHA256}
)
FetchContent_MakeAvailable(qtmesh_libsodium_src)

set(QTMESH_LIBSODIUM_PREFIX "${CMAKE_BINARY_DIR}/libsodium-install")
set(QTMESH_LIBSODIUM_MARKER "${QTMESH_LIBSODIUM_PREFIX}/lib/libsodium.a")

if(NOT EXISTS "${QTMESH_LIBSODIUM_MARKER}")
    message(STATUS "libsodium: configuring and building static library (one-time)")
    execute_process(
        COMMAND "${qtmesh_libsodium_src_SOURCE_DIR}/configure"
                "--prefix=${QTMESH_LIBSODIUM_PREFIX}"
                "--enable-static"
                "--disable-shared"
        WORKING_DIRECTORY "${qtmesh_libsodium_src_SOURCE_DIR}"
        RESULT_VARIABLE qtmesh_sodium_configure_result
    )
    if(NOT qtmesh_sodium_configure_result EQUAL 0)
        message(FATAL_ERROR "libsodium configure failed (${qtmesh_sodium_configure_result})")
    endif()
    execute_process(
        COMMAND make -j
        WORKING_DIRECTORY "${qtmesh_libsodium_src_SOURCE_DIR}"
        RESULT_VARIABLE qtmesh_sodium_build_result
    )
    if(NOT qtmesh_sodium_build_result EQUAL 0)
        message(FATAL_ERROR "libsodium build failed (${qtmesh_sodium_build_result})")
    endif()
    execute_process(
        COMMAND make install
        WORKING_DIRECTORY "${qtmesh_libsodium_src_SOURCE_DIR}"
        RESULT_VARIABLE qtmesh_sodium_install_result
    )
    if(NOT qtmesh_sodium_install_result EQUAL 0)
        message(FATAL_ERROR "libsodium install failed (${qtmesh_sodium_install_result})")
    endif()
endif()

add_library(qtmesh_sodium STATIC IMPORTED GLOBAL)
set_target_properties(qtmesh_sodium PROPERTIES
    IMPORTED_LOCATION "${QTMESH_LIBSODIUM_MARKER}"
    INTERFACE_INCLUDE_DIRECTORIES "${QTMESH_LIBSODIUM_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC=1;QTMESH_MINISIGN_VERIFY=1"
)
