if(NOT ENABLE_PS1_RIP)
    return()
endif()

set(PS1_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/src/PS1/runtime")

add_library(qtmesh_ps1core_libretro MODULE
    LibretroEmuCore.cpp
    LibretroEmuCorePlugin.cpp
    LibretroHost.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxBiosValidator.cpp
    ${CMAKE_SOURCE_DIR}/src/PS1/runtime/PsxDiscResolver.cpp
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

if(BUILD_TESTS AND TARGET UnitTests)
    add_dependencies(UnitTests qtmesh_ps1core_libretro)
endif()

if(BUILD_QT_MESH_EDITOR AND TARGET ${CMAKE_PROJECT_NAME})
    add_dependencies(${CMAKE_PROJECT_NAME} qtmesh_ps1core_libretro)
endif()
