##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
about.cpp
main.cpp
Manager.cpp
material.cpp
materialeditor.cpp
mainwindow.cpp
MeshTransform.cpp
OgreWidget.cpp
QtInputManager.cpp
SpaceCamera.cpp
SkeletonDebug.cpp
SkeletonTransform.cpp
MeshImporterExporter.cpp
EditorViewport.cpp
RotationGizmo.cpp
TranslationGizmo.cpp
TransformOperator.cpp
TransformWidget.cpp
PrimitivesWidget.cpp
PrimitiveObject.cpp
ViewportGrid.cpp
AnimationWidget.cpp
SelectionSet.cpp
SelectionBoxObject.cpp
ObjectItemModel.cpp
MaterialWidget.cpp
MaterialComboDelegate.cpp
MaterialHighlighter.cpp
)

set(HEADER_FILES
GlobalDefinitions.h
Euler.h
about.h
mainwindow.h
Manager.h
material.h
materialeditor.h
MeshTransform.h
OgreWidget.h
QtInputManager.h
QtKeyListener.h
QtMouseListener.h
SpaceCamera.h
SkeletonDebug.h
SkeletonTransform.h
MeshImporterExporter.h
EditorViewport.h
RotationGizmo.h
TranslationGizmo.h
TransformOperator.h
TransformWidget.h
PrimitivesWidget.h
PrimitiveObject.h
ViewportGrid.h
AnimationWidget.h
SelectionSet.h
SelectionBoxObject.h
ObjectItemModel.h
MaterialWidget.h
MaterialComboDelegate.h
MaterialHighlighter.h
)

ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/OgreXML")

#file(GLOB UI_FILES ./ui_files/*.ui)
# if we don't include this CMake will not include ui headers properly:
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${BUILD_INCLUDE_DIR} ${BUILD_UIH_DIR})

##############################################################
#  Processing ogre-procedural
##############################################################
#ADD_SUBDIRECTORY(ogre-procedural)
#include_directories("${OgreProcedural_SOURCE_DIR}/library/include" "${OgreProcedural_BINARY_DIR}/include")


##############################################################
#  Adding Resources
##############################################################
qt_add_resources(RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/resource.qrc")

#if(WIN32)
        file(GLOB RES "${CMAKE_CURRENT_SOURCE_DIR}/../resources/*.rc")
        file(GLOB PNG "${CMAKE_CURRENT_SOURCE_DIR}/../resources/*.png")
        file(GLOB ICO "${CMAKE_CURRENT_SOURCE_DIR}/../resources/*.ico")
        set(RESOURCE_SRCS ${RESOURCE_SRCS} ${RES} ${PNG} ${ICO})
#endif(WIN32)


##############################################################
#  Building the executable
##############################################################
if(WIN32)
        add_executable(${CMAKE_PROJECT_NAME} WIN32
	${HEADER_FILES}
	${SRC_FILES}
        ${RESOURCE_SRCS}
        )
else()
        add_executable(${CMAKE_PROJECT_NAME}
	${HEADER_FILES}
	${SRC_FILES}
	${RESOURCE_SRCS}
	)
endif()

##############################################################
#  Linking the executable
##############################################################
if(UNIX)
    target_link_libraries(${CMAKE_PROJECT_NAME}
    ${ASSIMP_LIBRARIES}
    ${OGRE_LIBRARIES}
    ${OgreProcedural_LIBRARY}
    "${OGRE_Codec_Assimp_LIBRARY_REL}" #TODO: find a better way to link ogreassimp
    Qt::Gui
    Qt::Core
    Qt::Widgets
    )
    IF(NOT Qt6_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} Qt5::X11Extras)
    ENDIF()
elseif(WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME}
    "${OGRE_Codec_Assimp_LIBRARY_REL}" #TODO: find a better way to link ogreassimp
    ${ASSIMP_LIBRARIES}
    ${OGRE_LIBRARIES}
    ${OgreProcedural_LIBRARY}
    Qt::Gui
    Qt::Core
    Qt::Widgets
    )
endif()

##############################################################
#  make sure that UI files will be processed first
##############################################################
ADD_DEPENDENCIES(${CMAKE_PROJECT_NAME}
ui
)

##############################################################
#  Install libraries
##############################################################
IF(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        IF(OGRE_Plugin_CgProgramManager_INCLUDE_DIR)
            SET(OGRELIBLIST cg
                Plugin_CgProgramManager_d
            )
        ENDIF()
        SET(OGRELIBLIST ${OGRELIBLIST}
            OgreMain_d
            Codec_Assimp_d
            Codec_STBI_d
            RenderSystem_GL_d
            Plugin_OctreeSceneManager_d
            Plugin_BSPSceneManager_d
            Plugin_ParticleFX_d
            Plugin_PCZSceneManager_d
            Plugin_OctreeZone_d
            OgrePaging_d
            OgreProperty_d
            OgreTerrain_d
            OgreRTShaderSystem_d
            OgreVolume_d
            OgreOverlay_d)

        FOREACH(ogrelib ${OGRELIBLIST})
            INSTALL(FILES ${OGRE_PLUGIN_DIR_DBG}/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ENDFOREACH(ogrelib)
    else()
        IF(OGRE_Plugin_CgProgramManager_INCLUDE_DIR)
            SET(OGRELIBLIST cg
                Plugin_CgProgramManager
            )
        ENDIF()
        SET(OGRELIBLIST ${OGRELIBLIST}
            libzlib
            OgreMain
            Codec_Assimp
            Codec_STBI
            RenderSystem_GL
            Plugin_OctreeSceneManager
            Plugin_BSPSceneManager
            Plugin_ParticleFX
            Plugin_PCZSceneManager
            Plugin_OctreeZone
            OgrePaging
            OgreProperty
            OgreTerrain
            OgreRTShaderSystem
            OgreVolume
            OgreOverlay)

        FOREACH(ogrelib ${OGRELIBLIST})
            INSTALL(FILES $ENV{OGRE_HOME}/bin/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ENDFOREACH(ogrelib)
    endif()

    GET_FILENAME_COMPONENT(ASSIMP_DLL_DIR ${ASSIMP_INCLUDE_DIR} PATH)
    find_file(ASSIMP_DLL_FILE NAMES libassim.dll assimp.dll Assimp32.dll PATHS "${ASSIMP_DLL_DIR}/../bin" "${ASSIMP_DLL_DIR}/../bin/x86")

    INSTALL(FILES ${ASSIMP_DLL_FILE} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    INSTALL(FILES ${OgreProcedural_BINARY_REL} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

endif(WIN32)
