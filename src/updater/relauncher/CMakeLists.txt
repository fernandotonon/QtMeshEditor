if(NOT ENABLE_AUTO_UPDATER)
    return()
endif()

set(_QTMESH_RELAUNCHER_OUTPUT "${CMAKE_BINARY_DIR}/bin/qtmesh-relauncher")

if(WIN32)
    add_executable(qtmesh-relauncher windows/relauncher.c)
    set_target_properties(qtmesh-relauncher PROPERTIES
        OUTPUT_NAME qtmesh-relauncher
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_link_libraries(qtmesh-relauncher PRIVATE kernel32 shell32)
    install(TARGETS qtmesh-relauncher RUNTIME DESTINATION bin COMPONENT Runtime)
else()
    if(APPLE)
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/macos/qtmesh-relauncher.sh"
            "${_QTMESH_RELAUNCHER_OUTPUT}"
            COPYONLY
        )
        install(PROGRAMS macos/qtmesh-relauncher.sh
                DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtMeshEditor.app/Contents/MacOS"
                COMPONENT Runtime
                RENAME qtmesh-relauncher)
    else()
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/linux/qtmesh-relauncher.sh"
            "${_QTMESH_RELAUNCHER_OUTPUT}"
            COPYONLY
        )
        install(PROGRAMS linux/qtmesh-relauncher.sh
                DESTINATION bin COMPONENT Runtime
                RENAME qtmesh-relauncher)
    endif()

    file(CHMOD "${_QTMESH_RELAUNCHER_OUTPUT}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

    add_custom_target(qtmesh-relauncher ALL DEPENDS "${_QTMESH_RELAUNCHER_OUTPUT}")
endif()
