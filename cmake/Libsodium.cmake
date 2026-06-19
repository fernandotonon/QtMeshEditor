# libsodium for minisign verify (#440 / #445).
# Linux + macOS: system pkg-config or autotools static build from source.
# Windows MinGW: official prebuilt static libs (same 1.0.20-stable release).

if(TARGET qtmesh_sodium)
    return()
endif()

function(qtmesh_sodium_add_imported_static lib_path include_dir)
    add_library(qtmesh_sodium STATIC IMPORTED GLOBAL)
    set_target_properties(qtmesh_sodium PROPERTIES
        IMPORTED_LOCATION "${lib_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${include_dir}"
        INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC=1;QTMESH_MINISIGN_VERIFY=1"
    )
    if(WIN32)
        set_property(TARGET qtmesh_sodium APPEND PROPERTY
            IMPORTED_LINK_INTERFACE_LIBRARIES "advapi32;bcrypt")
    endif()
endfunction()

if(WIN32)
    if(NOT MINGW)
        message(STATUS "libsodium: Windows MSVC minisign verify not wired — verify disabled")
        add_library(qtmesh_sodium INTERFACE)
        target_compile_definitions(qtmesh_sodium INTERFACE QTMESH_MINISIGN_VERIFY=0)
        return()
    endif()

    include(FetchContent)

    set(QTMESH_LIBSODIUM_MINGW_URL
        "https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable-mingw.tar.gz"
        CACHE STRING "Prebuilt MinGW libsodium tarball for minisign verify")
    set(QTMESH_LIBSODIUM_MINGW_SHA256
        "19f7e5f814f62f5bfdf6ea2208244adbddb80e73362879ddb0e4844bc1f12c82"
        CACHE STRING "SHA256 of MinGW libsodium tarball")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(QTMESH_LIBSODIUM_MINGW_ARCH "win64")
    else()
        set(QTMESH_LIBSODIUM_MINGW_ARCH "win32")
    endif()

    message(STATUS "libsodium: using prebuilt MinGW ${QTMESH_LIBSODIUM_MINGW_ARCH} package")

    FetchContent_Declare(
        qtmesh_libsodium_mingw
        URL ${QTMESH_LIBSODIUM_MINGW_URL}
        URL_HASH SHA256=${QTMESH_LIBSODIUM_MINGW_SHA256}
    )
    FetchContent_MakeAvailable(qtmesh_libsodium_mingw)

    set(QTMESH_LIBSODIUM_MINGW_ROOT
        "${qtmesh_libsodium_mingw_SOURCE_DIR}/libsodium-${QTMESH_LIBSODIUM_MINGW_ARCH}")
    set(QTMESH_LIBSODIUM_MARKER "${QTMESH_LIBSODIUM_MINGW_ROOT}/lib/libsodium.a")

    if(NOT EXISTS "${QTMESH_LIBSODIUM_MARKER}")
        message(FATAL_ERROR "libsodium MinGW package missing ${QTMESH_LIBSODIUM_MARKER}")
    endif()

    qtmesh_sodium_add_imported_static(
        "${QTMESH_LIBSODIUM_MARKER}"
        "${QTMESH_LIBSODIUM_MINGW_ROOT}/include")
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
include(ProcessorCount)

ProcessorCount(QTMESH_LIBSODIUM_JOBS)
if(NOT QTMESH_LIBSODIUM_JOBS OR QTMESH_LIBSODIUM_JOBS LESS 1)
    set(QTMESH_LIBSODIUM_JOBS 1)
endif()

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
        COMMAND make -j${QTMESH_LIBSODIUM_JOBS}
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

qtmesh_sodium_add_imported_static(
    "${QTMESH_LIBSODIUM_MARKER}"
    "${QTMESH_LIBSODIUM_PREFIX}/include")
