# Bundled platform validation presets for qtmesh scan (--profile / profile: in config).
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/profiles
    FILES_MATCHING PATTERN "*.json")
