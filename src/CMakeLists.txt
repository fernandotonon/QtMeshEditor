##############################################################
#  adding the files
##############################################################

set(SRC_FILES 
about.cpp
animationcontrolwidget.cpp
animationcontrolslider.cpp
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
animationcontrolwidget.h
animationcontrolslider.h
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

set(TEST_SOURCES "")

ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/OgreXML")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/Assimp")

#file(GLOB UI_FILES ./ui_files/*.ui)
# if we don't include this CMake will not include ui headers properly:
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${BUILD_INCLUDE_DIR} ${BUILD_UIH_DIR})

##############################################################
#  Processing ogre-procedural
##############################################################
set(OgreProcedural_CONFIG_FREETYPE "PROCEDURAL_NO_FREETYPE")
set(OGRE_PROCEDURAL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ogre-procedural/library/")
set(OgreProcedural_TEMPLATES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ogre-procedural/CMake/Templates")
include_directories(${OGRE_PROCEDURAL_LIB_DIR}include)

configure_file(${OgreProcedural_TEMPLATES_DIR}/ProceduralPlatform.h.in ${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPlatform.h)

set(SRC_FILES
${SRC_FILES}
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralBoxGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralCapsuleGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralConeGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralCylinderGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralIcoSphereGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPlaneGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralRoundedBoxGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSphereGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTorusGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTorusKnotGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTubeGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralUtils.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralShape.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralShapeGenerators.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPath.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPathGenerators.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTrack.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralExtruder.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralLathe.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTriangulator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPrecompiledHeaders.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMultiShape.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMultiShapeGenerators.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralGeometryHelpers.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralBoolean.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSpringGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSVG.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralDebugRendering.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureGenerator.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureModifiers.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralNoise.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMeshModifiers.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureBuffer.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTriangleBuffer.cpp
${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPrismGenerator.cpp
)

set(HEADER_FILES
${HEADER_FILES}
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPlatform.h
${OGRE_PROCEDURAL_LIB_DIR}include/Procedural.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralMeshGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralBoxGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralCapsuleGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralConeGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralCylinderGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralIcoSphereGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPlaneGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralSphereGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralRoundedBoxGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTorusGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTorusKnotGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTubeGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralUtils.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralExtruder.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralLathe.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralShape.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralShapeGenerators.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralShapeGeneratorsBase.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPath.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPathGenerators.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPathGeneratorsBase.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTrack.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralSplines.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTriangulator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTriangleBuffer.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralStableHeaders.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralMultiShape.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralMultiShapeGenerators.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralGeometryHelpers.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralBoolean.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralSpringGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralSVG.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralDebugRendering.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTextureGenerator.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTextureModifiers.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralNoise.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralMeshModifiers.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralTextureBuffer.h
${OGRE_PROCEDURAL_LIB_DIR}include/ProceduralPrismGenerator.h
)

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

OPTION(BUILD_QT_MESH_EDITOR "Build the application Qt Mesh Editor" ON)
IF(BUILD_QT_MESH_EDITOR)
##############################################################
#  Building the executable
##############################################################
if(WIN32)
        add_executable(${CMAKE_PROJECT_NAME} WIN32
	${HEADER_FILES}
	${SRC_FILES}
        ${RESOURCE_SRCS}
        )
elseif(APPLE)
        add_executable(${CMAKE_PROJECT_NAME}
	${HEADER_FILES}
	${SRC_FILES}
        ${RESOURCE_SRCS}
        )
        set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES MACOSX_BUNDLE TRUE)
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
if(APPLE)
    SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/Codec_Assimp.dylib)
elseif(UNIX)
    SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/Codec_Assimp.so)
elseif(WIN32)
    SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/libCodec_Assimp.dll.a)
endif()

if(UNIX)
    IF(NOT Qt6_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} Qt5::X11Extras)
    ENDIF()
endif()
target_link_libraries(${CMAKE_PROJECT_NAME}
${OGRE_Codec_Assimp_LIBRARY_REL} #TODO: find a better way to link ogreassimp
${OGRE_LIBRARIES}
${ASSIMP_LIBRARIES}
Qt::Gui
Qt::Core
Qt::Widgets
Qt::Network
)
ENDIF()

##############################################################
#  build the tests
##############################################################

if(BUILD_TESTS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    
    foreach(src IN LISTS SRC_FILES)
        if(NOT src MATCHES "main.cpp")
            list(APPEND TEST_SOURCES ${src})
        endif() 
    endforeach()

    # Add all files ending with _test.cpp to the set
    file(GLOB TestFiles "*_test.cpp")
    foreach(TestFile ${TestFiles})
        list(APPEND TEST_SOURCES ${TestFile})
    endforeach()    

    # Create the test executable
    add_executable(UnitTests
    ${HEADER_FILES}
    ${TEST_SOURCES}
    ${RESOURCE_SRCS}
    )

    # Link against Google Test libraries
    target_link_libraries(UnitTests 
    gtest 
    gtest_main 
    gmock
    gmock_main
    ${OGRE_Codec_Assimp_LIBRARY_REL} #TODO: find a better way to link ogreassimp
    ${OGRE_LIBRARIES}
    ${ASSIMP_LIBRARIES}
    Qt::Widgets Qt::Core Qt::Gui Qt::Test Qt::Network)
    
    ADD_DEPENDENCIES(UnitTests ui)

    gtest_discover_tests(UnitTests)
endif()


##############################################################
#  make sure that UI files will be processed first
##############################################################
IF(BUILD_QT_MESH_EDITOR)
    ADD_DEPENDENCIES(${CMAKE_PROJECT_NAME} ui)
ENDIF()
##############################################################
#  Install libraries
##############################################################
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
        IF(WIN32)
            INSTALL(FILES ${OGRE_PLUGIN_DIR_DBG}/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ENDIF()
    ENDFOREACH(ogrelib)
else()
    IF(OGRE_Plugin_CgProgramManager_INCLUDE_DIR)
        SET(OGRELIBLIST cg
            Plugin_CgProgramManager
        )
    ENDIF()
    SET(OGRELIBLIST ${OGRELIBLIST}
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
    IF(APPLE) # this is just for test, it is not using OgreBites yet. TODO: remove
        SET(OGRELIBLIST ${OGRELIBLIST}
            OgreBites
            OgreBullet
            OgreMeshLodGenerator
            RenderSystem_GL3Plus
        )
    ENDIF()
    FOREACH(ogrelib ${OGRELIBLIST})
        IF(WIN32)
            install(FILES ${OGRE_PLUGIN_DIR}/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ELSEIF(APPLE)
            if(EXISTS ${OGRE_PLUGIN_DIR}/${ogrelib}.dylib)
                INSTALL(FILES ${OGRE_PLUGIN_DIR}/${ogrelib}.dylib DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtMeshEditor.app/Contents/MacOS)
                if(EXISTS ${OGRE_PLUGIN_DIR}/${ogrelib}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR}.dylib)
                    INSTALL(FILES ${OGRE_PLUGIN_DIR}/${ogrelib}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR}.dylib DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtMeshEditor.app/Contents/MacOS)
                endif()
            elseif(EXISTS ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.dylib)
                INSTALL(FILES ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.dylib DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtMeshEditor.app/Contents/MacOS)
                if(EXISTS ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR}.dylib)
                    INSTALL(FILES ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR}.dylib DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtMeshEditor.app/Contents/MacOS)
                endif()
            endif()
        ELSEIF(UNIX)
            IF(EXISTS ${OGRE_${ogrelib}_LIBRARY_REL})
                INSTALL(FILES ${OGRE_${ogrelib}_LIBRARY_REL}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
		INSTALL(FILES ${OGRE_${ogrelib}_LIBRARY_REL}.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION /usr/local/lib)
            ENDIF()
            IF(EXISTS ${OGRE_PLUGIN_DIR}/${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR})
                INSTALL(FILES ${OGRE_PLUGIN_DIR}/${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
		INSTALL(FILES ${OGRE_PLUGIN_DIR}/${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION /usr/local/lib)
            ENDIF()
            IF(EXISTS ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR})
                INSTALL(FILES ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
		INSTALL(FILES ${OGRE_PLUGIN_DIR}/../lib${ogrelib}.so.${OGRE_VERSION_MAJOR}.${OGRE_VERSION_MINOR} DESTINATION /usr/local/lib)
            ENDIF()
        ENDIF()
    ENDFOREACH(ogrelib)
endif()

##############################################################
#  Configure Info.plist for Mac OS X
##############################################################
if(APPLE)
    configure_file (
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        "${CMAKE_INSTALL_PREFIX}/Info.plist"
        @ONLY)
endif()
