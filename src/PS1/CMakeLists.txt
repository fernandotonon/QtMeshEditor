##############################################################
#  PlayStation mesh formats (TMD, etc.)
##############################################################

set(SRC_FILES
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/PS1TMD.cpp
${CMAKE_CURRENT_SOURCE_DIR}/PS1TIM.cpp
PARENT_SCOPE
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/PS1TMD.h
${CMAKE_CURRENT_SOURCE_DIR}/PS1TIM.h
PARENT_SCOPE
)
