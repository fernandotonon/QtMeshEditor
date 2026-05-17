##############################################################
#  PlayStation mesh formats (TMD, etc.)
##############################################################

set(SRC_FILES
${SRC_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/PS1MAT.cpp
${CMAKE_CURRENT_SOURCE_DIR}/PS1PLY.cpp
${CMAKE_CURRENT_SOURCE_DIR}/PS1RSD.cpp
${CMAKE_CURRENT_SOURCE_DIR}/PS1TMD.cpp
${CMAKE_CURRENT_SOURCE_DIR}/PS1TIM.cpp
)

set(HEADER_FILES
${HEADER_FILES}
${CMAKE_CURRENT_SOURCE_DIR}/PS1MAT.h
${CMAKE_CURRENT_SOURCE_DIR}/PS1PLY.h
${CMAKE_CURRENT_SOURCE_DIR}/PS1RSD.h
${CMAKE_CURRENT_SOURCE_DIR}/PS1TMD.h
${CMAKE_CURRENT_SOURCE_DIR}/PS1TIM.h
)

if(ENABLE_PS1_RIP)
    list(APPEND SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuCoreLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuViewport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipLegalityDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipSessionWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipWorker.cpp
    )
    list(APPEND HEADER_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuCore.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuCoreLoader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuFramebuffer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuHooks.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/EmuViewport.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/IEmuCorePlugin.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipLegalityDialog.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipSessionWindow.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/PS1RipWorker.h
    )
endif()

set(SRC_FILES ${SRC_FILES} PARENT_SCOPE)
set(HEADER_FILES ${HEADER_FILES} PARENT_SCOPE)
