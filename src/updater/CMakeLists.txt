set(QTMESH_UPDATER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../UpdateVersion.cpp
    InstallFlavor.cpp
    ArtifactResolver.cpp
    UpdateVerifier.cpp
    MinisignVerify.cpp
    GitHubReleaseParser.cpp
    UpdaterWorker.cpp
    UpdaterController.cpp
    UpdaterInstaller.cpp
    UpdaterTelemetry.cpp
)

add_library(qtmesh_updater STATIC ${QTMESH_UPDATER_SOURCES})
set_target_properties(qtmesh_updater PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(qtmesh_updater
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(qtmesh_updater
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Qml
        qtmesh_sodium
)

if(BUILD_TESTS)
    target_compile_definitions(qtmesh_updater PRIVATE QTMESH_UNIT_TESTS)
endif()

if(ENABLE_AUTO_UPDATER AND BUILD_QT_MESH_EDITOR)
    add_subdirectory(relauncher)
endif()
