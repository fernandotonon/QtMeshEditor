##############################################################
#  adding the files
##############################################################

set(SRC_FILES
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/FBXExporter.cpp
PARENT_SCOPE
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/FBXExporter.h
PARENT_SCOPE
)

set(TEST_SOURCES
${TEST_SOURCES}
${CMAKE_CURRENT_SOURCE_DIR}/FBXExporter_test.cpp
PARENT_SCOPE
)
