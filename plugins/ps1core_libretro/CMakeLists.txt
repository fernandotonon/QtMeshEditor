if(NOT ENABLE_PS1_RIP)
    return()
endif()

set(PS1_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/src/PS1/runtime")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ps1core_libretro.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/ps1core_libretro.json
    @ONLY
)

add_library(qtmesh_ps1core_libretro MODULE
    LibretroEmuCore.cpp
    LibretroEmuCorePlugin.cpp
    LibretroHost.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/Gp0CaptureStats.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/Gp0HookDispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxGp0ChainRootScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/GteCapture.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxGteEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxGteInstructionCapture.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxMipsGteRunner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxGteRamScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxGp0ChainWalker.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxOrderingTableScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/GpuCommandParser.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxCaptureFilters.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxBiosValidator.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxDiscResolver.cpp
    # Model-space RAM scanners (#674) + their reconstruction deps. Referenced
    # by Gp0HookDispatch::captureFrameFromSystemRam; without these the module
    # links on Linux (lazy dlopen resolution) but fails macOS ld, which
    # rejects undefined symbols in MODULE libraries.
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxTmdRamScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxHmdRamScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/MeshReconstructorTexKeys.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/GteInverse.cpp
)

target_include_directories(qtmesh_ps1core_libretro PRIVATE
    ${PS1_RUNTIME_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(qtmesh_ps1core_libretro PRIVATE Qt6::Core)

set_target_properties(qtmesh_ps1core_libretro PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
)

add_custom_command(TARGET qtmesh_ps1core_libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/ps1core_libretro.json
        $<TARGET_FILE_DIR:qtmesh_ps1core_libretro>/ps1core_libretro.json
    COMMAND ${CMAKE_COMMAND} -E env
        bash "${CMAKE_SOURCE_DIR}/scripts/install-ps1-libretro-core.sh"
        "$<TARGET_FILE_DIR:qtmesh_ps1core_libretro>"
    COMMENT "Installing mednafen_psx_libretro into PS1Cores (if missing)"
)

# Optionally build the rip-instrumented beetle fork (#813) so in-core GTE/GP0
# capture works out of the box. OFF by default: it needs git + a working core
# toolchain (GNU make); plain ENABLE_PS1_RIP builds keep working against the
# stock downloaded core (RAM-scan capture paths unchanged).
option(ENABLE_PS1_RIP_CORE_BUILD "Build the rip-instrumented beetle fork into PS1Cores/" OFF)
if(ENABLE_PS1_RIP_CORE_BUILD)
    add_custom_command(TARGET qtmesh_ps1core_libretro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
            bash "${CMAKE_SOURCE_DIR}/scripts/build-ps1-rip-core.sh"
            "$<TARGET_FILE_DIR:qtmesh_ps1core_libretro>"
        COMMENT "Building beetle_psx_qtmesh_libretro (rip fork) into PS1Cores"
    )
endif()

if(BUILD_TESTS AND TARGET UnitTests)
    add_dependencies(UnitTests qtmesh_ps1core_libretro)
endif()

if(BUILD_QT_MESH_EDITOR AND TARGET ${CMAKE_PROJECT_NAME})
    add_dependencies(${CMAKE_PROJECT_NAME} qtmesh_ps1core_libretro)
endif()
