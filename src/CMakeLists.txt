##############################################################
#  adding the files
##############################################################

set(SRC_FILES
about.cpp
AppConsoleLog.cpp
AppLaunchHandler.cpp
AnimationBlender.cpp
AnimationControlController.cpp
CurveEditModel.cpp
CurveResampler.cpp
main.cpp
Manager.cpp
material.cpp
MaterialEditorQML.cpp
mainwindow.cpp
MeshTransform.cpp
OgreWidget.cpp
QtInputManager.cpp
SpaceCamera.cpp
SkeletonDebug.cpp
SkeletonTransform.cpp
MeshImporterExporter.cpp
EditorViewport.cpp
ViewportTitleBar.cpp
RotationGizmo.cpp
TranslationGizmo.cpp
ScaleGizmo.cpp
BevelGizmo.cpp
TransformOperator.cpp
PrimitivesWidget.cpp
PrimitiveObject.cpp
ViewportGrid.cpp
AnimationWidget.cpp
SelectionSet.cpp
SelectionBoxObject.cpp
ObjectItemModel.cpp
MaterialComboDelegate.cpp
MaterialHighlighter.cpp
QMLMaterialHighlighter.cpp
LLMWorker.cpp
LLMManager.cpp
ModelDownloader.cpp
LLMSettingsWidget.cpp
MCPServer.cpp
MCPSettingsDialog.cpp
SentryReporter.cpp
BoneWeightOverlay.cpp
NormalVisualizer.cpp
AnimationMerger.cpp
CLIPipeline.cpp
ConsoleLogSanitize.cpp
MeshInfoOverlay.cpp
RTShaderHelper.cpp
ViewCube/ViewCubeController.cpp
SDWorker.cpp
SDManager.cpp
UndoManager.cpp
SubMeshTransform.cpp
SubEntityHighlight.cpp
commands/TransformCommands.cpp
commands/MoveKeyframeCommand.cpp
commands/BulkKeyframeCommands.cpp
commands/SetKeyframeValueCommand.cpp
commands/ResampleCurveCommand.cpp
commands/CurveEditModelChangeCommand.cpp
commands/DecimateTrackCommand.cpp
commands/BoneTransformCommand.cpp
commands/AddKeyframeCommand.cpp
commands/ApplyMaterialCommand.cpp
commands/DeleteKeyframeCommand.cpp
commands/MorphCommands.cpp
commands/NodeAnimCommands.cpp
commands/PoseLibraryCommands.cpp
commands/SkeletonResolver.cpp
commands/ComputeSkinWeightsCommand.cpp
commands/AutoRigCommand.cpp
commands/UVEditCommand.cpp
commands/UvSeamCommands.cpp
UvSeamData.cpp
UvSeamOps.cpp
BoneDragRelease.cpp
PropertiesPanelController.cpp
SceneTreeModel.cpp
ThemeManager.cpp
BatchExporter.cpp
MaterialPresetLibrary.cpp
MeshLodController.cpp
MeshOptimizerLod.cpp
ExportOptimizer.cpp
UvUnwrap.cpp
UvUnwrapController.cpp
UVEditorController.cpp
UVTransform.cpp
QuadRetopo.cpp
QuadRetopoController.cpp
SkinWeights.cpp
SkinWeightsController.cpp
AutoRig.cpp
AutoRigController.cpp
UniRigPredictor.cpp
MotionInbetween.cpp
MotionLibrary.cpp
MotionGenerator.cpp
MeshSegmenter.cpp
MeshDepthRenderer.cpp
MultiViewTextureBaker.cpp
TextureChannelPacker.cpp
TextureAtlasPacker.cpp
PaintBufferImageProvider.cpp
PaintSelectionMask.cpp
TexturePaintBuffer.cpp
TexturePaintController.cpp
VertexColorBaker.cpp
VATBaker.cpp
VATBakerController.cpp
VATShaderEmitter.cpp
IsometricSpritesController.cpp
MinimalEXRWriter.cpp
MorphAnimationManager.cpp
NodeAnimationManager.cpp
PoseLibrary.cpp
ApplyAtlas.cpp
EmbeddedTextureCache.cpp
NormalMapGenerator.cpp
PbrMapSynth.cpp
TextureUpscaler.cpp
AIAssistManager.cpp
MemoryEstimator.cpp
DrawCallAnalyzer.cpp
VertexCacheOptimizer.cpp
MeshDecimator.cpp
MeshDecimatorController.cpp
MeshValidator.cpp
AIChatManager.cpp
WelcomeScreenController.cpp
WelcomeDialog.cpp
ScanConfig.cpp
ScanEngine.cpp
PlatformProfile.cpp
QtMeshCloudClient.cpp
CloudCredentialStore.cpp
CloudUploadPlanner.cpp
CloudAccountMenuButton.cpp
DependencyResolver.cpp
ProjectPackager.cpp
QtMeshCloudSession.cpp
CloudUploadDialog.cpp
CloudProjectsController.cpp
CloudDeepLink.cpp
CloudUploadProgress.cpp
CloudCLIPipeline.cpp
FeedbackDiagnostics.cpp
FeedbackDialog.cpp
FeedbackReportHelper.cpp
AssetBrowserController.cpp
AssetScanController.cpp
MaterialPreviewRenderer.cpp
ModelTurntableRenderer.cpp
ModelIsometricRenderer.cpp
EditableMesh.cpp
EditModeController.cpp
EditorModeController.cpp
HalfEdgeMesh.cpp
)

set(HEADER_FILES
AnimationBlender.h
AnimationControlController.h
CurveEditModel.h
CurveResampler.h
ConsoleLogSanitize.h
GlobalDefinitions.h
Euler.h
about.h
AppConsoleLog.h
AppLaunchHandler.h
AppSettingsKeys.h
mainwindow.h
Manager.h
material.h
MaterialEditorQML.h
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
ViewportTitleBar.h
RotationGizmo.h
TranslationGizmo.h
ScaleGizmo.h
BevelGizmo.h
TransformOperator.h
PrimitivesWidget.h
PrimitiveObject.h
ViewportGrid.h
ViewportSettingsKeys.h
AnimationWidget.h
SelectionSet.h
SelectionBoxObject.h
ObjectItemModel.h
MaterialComboDelegate.h
MaterialHighlighter.h
QMLMaterialHighlighter.h
LLMWorker.h
LLMManager.h
ModelDownloader.h
LLMSettingsWidget.h
MCPServer.h
MCPSettingsDialog.h
SentryReporter.h
BoneWeightOverlay.h
NormalVisualizer.h
AnimationMerger.h
CLIPipeline.h
MeshInfoOverlay.h
RTShaderHelper.h
ViewCube/ViewCubeController.h
SDWorker.h
SDManager.h
UndoManager.h
SubMeshTransform.h
SubEntityHighlight.h
commands/TransformCommands.h
commands/ApplyMaterialCommand.h
PropertiesPanelController.h
SceneTreeModel.h
ThemeManager.h
BatchExporter.h
MaterialPresetLibrary.h
MeshLodController.h
MeshOptimizerLod.h
ExportOptimizer.h
UvUnwrap.h
UvUnwrapController.h
UVEditorController.h
UvSeamData.h
UvSeamOps.h
commands/UvSeamCommands.h
QuadRetopo.h
QuadRetopoController.h
SkinWeights.h
SkinWeightsController.h
AutoRig.h
AutoRigController.h
UniRigPredictor.h
MotionInbetween.h
MotionLibrary.h
MotionGenerator.h
MeshSegmenter.h
MeshDepthRenderer.h
MultiViewTextureBaker.h
TextureChannelPacker.h
TextureAtlasPacker.h
PaintBufferImageProvider.h
PaintSelectionMask.h
TexturePaintBuffer.h
UpdateVersion.h
TexturePaintController.h
VertexColorBaker.h
ApplyAtlas.h
EmbeddedTextureCache.h
NormalMapGenerator.h
MemoryEstimator.h
DrawCallAnalyzer.h
VertexCacheOptimizer.h
MeshDecimator.h
MeshDecimatorController.h
MeshValidator.h
AIChatManager.h
WelcomeScreenController.h
WelcomeDialog.h
ScanConfig.h
ScanEngine.h
PlatformProfile.h
QtMeshCloudClient.h
CloudCredentialStore.h
CloudUploadPlanner.h
DependencyResolver.h
ProjectPackager.h
QtMeshCloudSession.h
CloudUploadDialog.h
CloudProjectsController.h
CloudDeepLink.h
CloudUploadProgress.h
CloudCLIPipeline.h
FeedbackPrefill.h
FeedbackDiagnostics.h
FeedbackDialog.h
FeedbackReportHelper.h
AssetBrowserController.h
AssetScanController.h
MaterialPreviewRenderer.h
ModelTurntableRenderer.h
ModelIsometricRenderer.h
IsometricSpritesController.h
EditableMesh.h
EditModeController.h
EditorModeController.h
HalfEdgeMesh.h
)

set(TEST_SOURCES "")

ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/OgreXML")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/Assimp")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/FBX")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/PS1")

#file(GLOB UI_FILES ./ui_files/*.ui)
# if we don't include this CMake will not include ui headers properly:
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${BUILD_INCLUDE_DIR} ${BUILD_UIH_DIR})

if(ENABLE_AUTO_UPDATER OR BUILD_TESTS)
    add_subdirectory(updater)
endif()

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
#  Adding resources  
##############################################################
qt_add_resources(RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/resource.qrc")
qt_add_resources(VAT_SHADER_RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/vat-shaders/vat_shaders.qrc")

##############################################################
#  Adding QML Resources  
##############################################################
qt_add_resources(QML_RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/qml_resources.qrc")

##############################################################
#  Adding QML Resources  
##############################################################
# Set Qt policies to suppress warnings
if(POLICY QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()
if(POLICY QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# Temporarily disable qt_add_qml_module to use simple QRC approach
# qt_add_qml_module(${CMAKE_PROJECT_NAME}_qml
#     URI MaterialEditorQML
#     VERSION 1.0
#     OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/MaterialEditorQML
#     QML_FILES
#         qml/MaterialEditorWindow.qml
#         qml/PassPropertiesPanel.qml
#         qml/TexturePropertiesPanel.qml
#     SOURCES
#         MaterialEditorQML.h
#         MaterialEditorQML.cpp
# )

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
        ${QML_RESOURCE_SRCS}
        ${VAT_SHADER_RESOURCE_SRCS}
        MaterialEditorQML.h
        MaterialEditorQML.cpp
        )
elseif(APPLE)
        add_executable(${CMAKE_PROJECT_NAME}
	${HEADER_FILES}
	${SRC_FILES}
        ${RESOURCE_SRCS}
        ${QML_RESOURCE_SRCS}
        ${VAT_SHADER_RESOURCE_SRCS}
        MaterialEditorQML.h
        MaterialEditorQML.cpp
        )
        set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES MACOSX_BUNDLE TRUE)
else()
        add_executable(${CMAKE_PROJECT_NAME}
        ${HEADER_FILES}
        ${SRC_FILES}
        ${RESOURCE_SRCS}
        ${QML_RESOURCE_SRCS}
        ${VAT_SHADER_RESOURCE_SRCS}
        MaterialEditorQML.h
        MaterialEditorQML.cpp
        )
endif()

##############################################################
#  Create 'qtmesh' symlink/wrapper for CLI pipeline mode
##############################################################
if(APPLE)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/MacOS/${CMAKE_PROJECT_NAME}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qtmesh"
        COMMENT "Creating qtmesh symlink -> app bundle binary"
    )
elseif(WIN32)
    # Build a tiny qtmesh.exe launcher instead of a .cmd wrapper.
    # WinGet portable manifests don't allow .cmd files.
    add_executable(qtmesh qtmesh_launcher.cpp)
    set_target_properties(qtmesh PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
else()
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PROJECT_NAME}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qtmesh"
        COMMENT "Creating qtmesh symlink -> ${CMAKE_PROJECT_NAME}"
    )
endif()

# Dev runs expect `media/` beside the executable: resources.cfg uses
# media/materials/* and RTShaderHelper adds media/RTShaderLib + media/Main
# (OgreUnifiedShader.h). Without this copy the viewport can stay black.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/media
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/media
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/profiles
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/profiles
    COMMENT "Copying media/ and profiles/ next to QtMeshEditor"
)

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

# Check if Codec_Assimp exists (optional - QtMeshEditor has its own Assimp integration)
if(EXISTS "${OGRE_Codec_Assimp_LIBRARY_REL}")
    set(CODEC_ASSIMP_LINK_LIB ${OGRE_Codec_Assimp_LIBRARY_REL})
    message(STATUS "Found OGRE Codec_Assimp: ${OGRE_Codec_Assimp_LIBRARY_REL}")
else()
    set(CODEC_ASSIMP_LINK_LIB "")
    message(STATUS "OGRE Codec_Assimp not found, using built-in Assimp integration")
endif()

# Cloud session secrets are stored in QSettings (see CloudCredentialStore.cpp).
# The platform secret stores are no longer the backend, but the migration path
# still does a ONE-TIME read of the OS store to carry over a pre-QSettings
# session — macOS needs the Security framework and Windows needs advapi32 for
# that read. libsecret is fully dropped (no Linux secret-store read-back).

target_link_libraries(${CMAKE_PROJECT_NAME}
${CODEC_ASSIMP_LINK_LIB}
${OGRE_LIBRARIES}
${ASSIMP_LIBRARIES}
Qt::Gui
Qt::Core
Qt::Widgets
Qt::Network
Qt::Qml
Qt::Quick
Qt::QuickWidgets
Qt::QuickControls2
meshoptimizer
xatlas
qtmesh_sodium
)

if(ENABLE_AUTO_UPDATER)
    target_link_libraries(${CMAKE_PROJECT_NAME} qtmesh_updater)
endif()

# One-time legacy secret-store read for migration (see CloudCredentialStore).
if(APPLE)
    target_link_libraries(${CMAKE_PROJECT_NAME} "-framework Security")
elseif(WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME} advapi32)
endif()

# Link llama.cpp if enabled
if(ENABLE_LOCAL_LLM)
    target_link_libraries(${CMAKE_PROJECT_NAME} llama ggml)
    if(APPLE)
        target_link_libraries(${CMAKE_PROJECT_NAME}
            "-framework Metal"
            "-framework Foundation"
            "-framework MetalKit"
        )
    endif()
endif()

# Link stable-diffusion.cpp if enabled
if(ENABLE_STABLE_DIFFUSION)
    target_link_libraries(${CMAKE_PROJECT_NAME} stable-diffusion)
endif()

# Link ONNX Runtime if enabled (#404 — AI PBR map synthesis). The imported
# target is a SHARED lib, so copy it next to the binary at build time or it
# won't be found at runtime.
if(ENABLE_ONNX)
    target_link_libraries(${CMAKE_PROJECT_NAME} qtmesh_onnx)
    if(QTMESH_ONNX_LIB_DIR)
        # Copy the versioned lib AND its SONAME symlinks (libonnxruntime.so.1 /
        # .so) — the loader requests the SONAME, not the resolved versioned name,
        # so copying only one file breaks `.deb`/Docker packaging.
        file(GLOB _ort_runtime_libs
            "${QTMESH_ONNX_LIB_DIR}/libonnxruntime.so*"
            "${QTMESH_ONNX_LIB_DIR}/libonnxruntime*.dylib"
            "${QTMESH_ONNX_LIB_DIR}/onnxruntime.dll")
        foreach(_ortlib ${_ort_runtime_libs})
            add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ortlib}" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
                COMMENT "Copying ONNX Runtime lib ${_ortlib} next to QtMeshEditor")
        endforeach()
    endif()
endif()

# Link Sentry if enabled
if(ENABLE_SENTRY)
    target_link_libraries(${CMAKE_PROJECT_NAME} sentry)
endif()

if(ENABLE_PS1_RIP AND TARGET Qt6::Gamepad)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_QT_GAMEPAD)
    target_link_libraries(${CMAKE_PROJECT_NAME} Qt6::Gamepad)
endif()

if(ENABLE_AUTO_UPDATER AND TARGET qtmesh-relauncher)
    add_dependencies(${CMAKE_PROJECT_NAME} qtmesh-relauncher)
    if(APPLE)
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_BINARY_DIR}/bin/qtmesh-relauncher"
                "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/MacOS/qtmesh-relauncher"
            COMMENT "Copying qtmesh-relauncher into app bundle"
        )
    endif()
endif()
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

    # Add all files ending with _test.cpp to the set (recursively to include subdirectories like Assimp)
    file(GLOB_RECURSE TestFiles "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    foreach(TestFile ${TestFiles})
        # The PS1 runtime sources (src/PS1/runtime/*.cpp) are only added to
        # SRC_FILES when ENABLE_PS1_RIP is ON, so their *_test.cpp counterparts
        # must be gated the same way — otherwise the test links a stub test
        # (e.g. TextureDecoder_test.cpp) against a missing impl and fails with
        # undefined symbols like qHash(TextureDecoder::MaterialKey).
        if(NOT ENABLE_PS1_RIP AND TestFile MATCHES "/PS1/runtime/")
            continue()
        endif()
        list(APPEND TEST_SOURCES ${TestFile})
    endforeach()

    # Include legacy widget sources that are only exercised by unit tests.
    list(APPEND TEST_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/animationcontrolslider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animationcontrolslider.h
    )

    # Create the test executable (uses custom main for gcov signal handlers)
    add_executable(UnitTests
    ${HEADER_FILES}
    ${TEST_SOURCES}
    ${RESOURCE_SRCS}
    ${QML_RESOURCE_SRCS}
    # Compile the VAT shader qrc into the test binary so VATShaderEmitter's
    # writeShaders() tests run for real instead of GTEST_SKIP-ing (the CI
    # harness treats a suite that only skips as a failure).
    ${VAT_SHADER_RESOURCE_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
    )
    target_compile_definitions(UnitTests PRIVATE BATCH_EXPORTER_TEST_SEAM QTMESH_UNIT_TESTS
        "QTMESH_UT_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")

    # Link against Google Test libraries (no gtest_main - we provide our own main)
    target_link_libraries(UnitTests
    gtest
    gmock
    ${CODEC_ASSIMP_LINK_LIB}
    ${OGRE_LIBRARIES}
    ${ASSIMP_LIBRARIES}
    Qt::Widgets Qt::Core Qt::Gui Qt::Test Qt::Network Qt::Qml Qt::Quick Qt::QuickWidgets Qt::QuickControls2
    meshoptimizer xatlas qtmesh_sodium qtmesh_updater)

    # CloudCredentialStore.cpp (compiled into the tests) does a one-time legacy
    # secret-store read in migrateLegacySettingsIfNeeded(), which
    # CloudCredentialStore_test.cpp exercises — so the test binary needs the
    # same platform link as the app or macOS/Windows test builds fail to link.
    if(APPLE)
        target_link_libraries(UnitTests "-framework Security")
    elseif(WIN32)
        target_link_libraries(UnitTests advapi32)
    endif()

    # Link llama.cpp for tests if enabled
    if(ENABLE_LOCAL_LLM)
        target_link_libraries(UnitTests llama ggml)
    endif()

    # Link stable-diffusion.cpp for tests if enabled
    if(ENABLE_STABLE_DIFFUSION)
        target_link_libraries(UnitTests stable-diffusion)
    endif()

    # Link ONNX Runtime for tests if enabled (#404)
    if(ENABLE_ONNX)
        target_link_libraries(UnitTests qtmesh_onnx)
        if(QTMESH_ONNX_LIB_DIR)
            file(GLOB _ort_test_libs
                "${QTMESH_ONNX_LIB_DIR}/libonnxruntime.so*"
                "${QTMESH_ONNX_LIB_DIR}/libonnxruntime*.dylib"
                "${QTMESH_ONNX_LIB_DIR}/onnxruntime.dll")
            foreach(_ortlib ${_ort_test_libs})
                add_custom_command(TARGET UnitTests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_ortlib}" "$<TARGET_FILE_DIR:UnitTests>"
                    COMMENT "Copying ONNX Runtime lib ${_ortlib} next to UnitTests")
            endforeach()
        endif()
    endif()

    # Link Sentry for tests if enabled
    if(ENABLE_SENTRY)
        target_link_libraries(UnitTests sentry)
    endif()

    ADD_DEPENDENCIES(UnitTests ui)

    if(ENABLE_PS1_RIP)
        add_dependencies(UnitTests qtmesh_ps1core_stub)
        if(TARGET qtmesh_ps1core_libretro)
            add_dependencies(UnitTests qtmesh_ps1core_libretro)
        endif()
        if(TARGET Qt6::Gamepad)
            target_compile_definitions(UnitTests PRIVATE HAVE_QT_GAMEPAD)
            target_link_libraries(UnitTests Qt6::Gamepad)
        endif()
    endif()

    # On macOS, create symlinks from Ogre plugin .framework bundles into the
    # UnitTests binary directory so that plugins.cfg (PluginFolder=../) can
    # find them.  The app bundle has these in Contents/MacOS/ via INSTALL
    # commands, but UnitTests is a plain executable — not a bundle.
    # Discover tests at ctest time (not build time) so CI can provide Xvfb/GL env.
    gtest_discover_tests(UnitTests DISCOVERY_MODE PRE_TEST)

    # Copy RTSS shader resources to the UnitTests binary directory so that
    # RTSSResourcesTest can find them without requiring cmake --install.
    # The test looks for media/RTShaderLib/ and media/Main/ relative to
    # applicationDirPath().
    add_custom_command(TARGET UnitTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/media/RTShaderLib
            $<TARGET_FILE_DIR:UnitTests>/media/RTShaderLib
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/media/Main
            $<TARGET_FILE_DIR:UnitTests>/media/Main
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/profiles
            $<TARGET_FILE_DIR:UnitTests>/profiles
        COMMENT "Copying RTSS shader resources and platform profiles to test binary directory"
    )
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
        OgreOverlay_d
        OgreMeshLodGenerator_d)

    FOREACH(ogrelib ${OGRELIBLIST})
        IF(WIN32)
            INSTALL(FILES ${OGRE_PLUGIN_DIR_DBG}/${ogrelib}.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        ENDIF()
    ENDFOREACH(ogrelib)
    IF(WIN32 AND OGRE_PLUGIN_DIR_DBG)
        file(GLOB _OGRE_WIN_RUNTIME_DLLS_DBG
            "${OGRE_PLUGIN_DIR_DBG}/Ogre*.dll"
            "${OGRE_PLUGIN_DIR_DBG}/Plugin_*.dll"
            "${OGRE_PLUGIN_DIR_DBG}/Codec_*.dll"
            "${OGRE_PLUGIN_DIR_DBG}/RenderSystem_*.dll"
            "${OGRE_PLUGIN_DIR_DBG}/cg.dll")
        if(_OGRE_WIN_RUNTIME_DLLS_DBG)
            install(FILES ${_OGRE_WIN_RUNTIME_DLLS_DBG}
                    DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        endif()
    ENDIF()
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
        OgreOverlay
        OgreMeshLodGenerator)
    IF(APPLE) # this is just for test, it is not using OgreBites yet. TODO: remove
        SET(OGRELIBLIST ${OGRELIBLIST}
            OgreBites
            OgreBullet
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
    # Windows: also install every Ogre runtime DLL beside the plugins dir so component
    # libraries (e.g. OgreMeshLodGenerator.dll) cannot be omitted from OGRELIBLIST.
    IF(WIN32 AND OGRE_PLUGIN_DIR)
        file(GLOB _OGRE_WIN_RUNTIME_DLLS
            "${OGRE_PLUGIN_DIR}/Ogre*.dll"
            "${OGRE_PLUGIN_DIR}/Plugin_*.dll"
            "${OGRE_PLUGIN_DIR}/Codec_*.dll"
            "${OGRE_PLUGIN_DIR}/RenderSystem_*.dll"
            "${OGRE_PLUGIN_DIR}/cg.dll")
        if(_OGRE_WIN_RUNTIME_DLLS)
            install(FILES ${_OGRE_WIN_RUNTIME_DLLS}
                    DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        endif()
    ENDIF()
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
