# Build a static libsodium for minisign verify (spike #440 / follow-up #445).
# Windows MinGW is not wired yet — MinisignVerify returns Unsupported there.

if(TARGET qtmesh_sodium)
    return()
endif()

include(FetchContent)

set(QTMESH_LIBSODIUM_URL
    "https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable.tar.gz"
    CACHE STRING "libsodium source tarball for minisign verify")
set(QTMESH_LIBSODIUM_SHA256
    "b6b1d2a8802cd8bfa611638c0e9ce31d14ef324e16062c39a76854091cba6d7f"
    CACHE STRING "SHA256 of libsodium tarball")

FetchContent_Declare(
    qtmesh_libsodium_src
    URL ${QTMESH_LIBSODIUM_URL}
    URL_HASH SHA256=${QTMESH_LIBSODIUM_SHA256}
)
FetchContent_MakeAvailable(qtmesh_libsodium_src)

set(QTMESH_LIBSODIUM_PREFIX "${CMAKE_BINARY_DIR}/libsodium-install")

if(NOT (UNIX AND NOT APPLE))
    message(STATUS "libsodium: skipped (minisign verify is Linux-only in spike #440)")
    add_library(qtmesh_sodium INTERFACE)
    target_compile_definitions(qtmesh_sodium INTERFACE QTMESH_MINISIGN_VERIFY=0)
    return()
endif()

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
