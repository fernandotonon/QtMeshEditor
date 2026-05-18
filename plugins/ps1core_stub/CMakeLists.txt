if(NOT ENABLE_PS1_RIP)
    return()
endif()

set(PS1_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/src/PS1/runtime")

add_library(qtmesh_ps1core_stub MODULE
    StubCaptureSynth.cpp
    StubEmuCore.cpp
    StubEmuCorePlugin.cpp
)

target_include_directories(qtmesh_ps1core_stub PRIVATE
    ${PS1_RUNTIME_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(qtmesh_ps1core_stub PRIVATE Qt6::Core)

set_target_properties(qtmesh_ps1core_stub PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/PS1Cores"
)

if(BUILD_TESTS AND TARGET UnitTests)
    add_dependencies(UnitTests qtmesh_ps1core_stub)
endif()

if(BUILD_QT_MESH_EDITOR AND TARGET ${CMAKE_PROJECT_NAME})
    add_dependencies(${CMAKE_PROJECT_NAME} qtmesh_ps1core_stub)
endif()
