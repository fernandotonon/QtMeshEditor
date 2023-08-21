##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/Importer.cpp
${CMAKE_CURRENT_SOURCE_DIR}/MaterialProcessor.cpp
PARENT_SCOPE
)

set(TEST_SOURCES 
${CMAKE_CURRENT_SOURCE_DIR}/MaterialProcessor_test.cpp
PARENT_SCOPE
)
