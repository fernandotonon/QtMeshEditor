##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/AssimpLoader.cpp
PARENT_SCOPE
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/OgreAssimpLoader.h
PARENT_SCOPE
)