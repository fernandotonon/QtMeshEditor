##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/tinystr.cpp
${CMAKE_CURRENT_SOURCE_DIR}/tinyxml.cpp
${CMAKE_CURRENT_SOURCE_DIR}/tinyxmlerror.cpp
${CMAKE_CURRENT_SOURCE_DIR}/tinyxmlparser.cpp
${CMAKE_CURRENT_SOURCE_DIR}/OgreXMLMeshSerializer.cpp
${CMAKE_CURRENT_SOURCE_DIR}/OgreXMLSkeletonSerializer.cpp
PARENT_SCOPE
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/tinystr.h
${CMAKE_CURRENT_SOURCE_DIR}/tinyxml.h
${CMAKE_CURRENT_SOURCE_DIR}/OgreXMLMeshSerializer.h
${CMAKE_CURRENT_SOURCE_DIR}/OgreXMLPrerequisites.h
${CMAKE_CURRENT_SOURCE_DIR}/OgreXMLSkeletonSerializer.h
PARENT_SCOPE
)
