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
#  Processing ogre-assimp
##############################################################
#ADD_SUBDIRECTORY(ogre-assimp)
#include_directories(${OGRE_ASSIMP_INCLUDE_DIRS})

##############################################################
#  Adding Resources
##############################################################
qt5_add_resources(RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/resource.qrc")

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
    qt5_use_modules(${CMAKE_PROJECT_NAME} Core Gui Widgets X11Extras)
    target_link_libraries(${CMAKE_PROJECT_NAME}
    ${ASSIMP_LIBRARIES}
    ${Boost_SYSTEM_LIBRARY}
    ${OGRE_LIBRARIES}
    ${OgreProcedural_LIBRARY}
    Qt5::Gui
    Qt5::Core
    Qt5::Widgets
    Qt5::X11Extras
    )
elseif(WIN32)
    qt5_use_modules(${CMAKE_PROJECT_NAME} Core Gui Widgets)
    target_link_libraries(${CMAKE_PROJECT_NAME}
    "${OGRE_DIR}/../bin/libCodec_Assimp.dll.a" #TODO: find a better way to link ogreassimp
    ${ASSIMP_LIBRARIES}
    ${Boost_SYSTEM_LIBRARY}
    ${OGRE_LIBRARIES}
    ${OgreProcedural_LIBRARY}
    Qt5::Gui
    Qt5::Core
    Qt5::Widgets
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
        SET(OGRELIBLIST
            OgreMain_d
            cg
            RenderSystem_GL_d
            Plugin_OctreeSceneManager_d
            Plugin_BSPSceneManager_d
            Plugin_CgProgramManager_d
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
        SET(OGRELIBLIST
            OgreMain
            OIS
            cg
            RenderSystem_GL
            Plugin_OctreeSceneManager
            Plugin_BSPSceneManager
            Plugin_CgProgramManager
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
            INSTALL(FILES $ENV{OGRE_HOME}/bin/Release/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ENDFOREACH(ogrelib)
    endif()

    GET_FILENAME_COMPONENT(ASSIMP_DLL_DIR ${ASSIMP_INCLUDE_DIR} PATH)
    find_file(ASSIMP_DLL_FILE NAMES libassim.dll assimp.dll Assimp32.dll PATHS "${ASSIMP_DLL_DIR}/../bin" "${ASSIMP_DLL_DIR}/../bin/x86")
    find_file(ASSIMP_EXE_FILE NAMES assimp.exe PATHS "${ASSIMP_DLL_DIR}/../bin/x86" "${ASSIMP_DLL_DIR}/../bin32")

    INSTALL(FILES ${ASSIMP_DLL_FILE} ${ASSIMP_EXE_FILE} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

endif(WIN32)
