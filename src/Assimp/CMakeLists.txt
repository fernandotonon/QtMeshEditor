##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/Importer.cpp
${CMAKE_CURRENT_SOURCE_DIR}/MaterialProcessor.cpp
${CMAKE_CURRENT_SOURCE_DIR}/AnimationProcessor.cpp
${CMAKE_CURRENT_SOURCE_DIR}/BoneProcessor.cpp
${CMAKE_CURRENT_SOURCE_DIR}/MeshProcessor.cpp
PARENT_SCOPE
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/Importer.h
${CMAKE_CURRENT_SOURCE_DIR}/MaterialProcessor.h
${CMAKE_CURRENT_SOURCE_DIR}/AnimationProcessor.h
${CMAKE_CURRENT_SOURCE_DIR}/BoneProcessor.h
${CMAKE_CURRENT_SOURCE_DIR}/MeshProcessor.h
PARENT_SCOPE
)

set(TEST_SOURCES 
${TEST_SOURCES}
${CMAKE_CURRENT_SOURCE_DIR}/MaterialProcessor_test.cpp
${CMAKE_CURRENT_SOURCE_DIR}/AnimationProcessor_test.cpp
PARENT_SCOPE
)
